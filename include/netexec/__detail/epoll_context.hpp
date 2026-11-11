// include/netexec/__detail/epoll_context.hpp                   -*-C++-*-
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef INCLUDED_NETEXEC_DETAIL_EPOLL_CONTEXT
#define INCLUDED_NETEXEC_DETAIL_EPOLL_CONTEXT

// ----------------------------------------------------------------------------

#include <netexec/__detail/platform.hpp>
#include <netexec/__detail/netfwd.hpp>
#include <netexec/__detail/container.hpp>
#include <netexec/__detail/context_base.hpp>
#include <netexec/__detail/sorted_list.hpp>

#if defined(NET_EXEC_USE_EPOLL)
#if !defined(__linux__)
#  error "NET_EXEC_USE_EPOLL requires Linux (epoll)"
#endif

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <system_error>
#include <unordered_map>

// ----------------------------------------------------------------------------

namespace netexec::detail {
struct epoll_context;
} // namespace netexec::detail

// ----------------------------------------------------------------------------
// epoll-based io_context backend (Linux only).
//
// Threading contract (same as the other backends): all mutating operations
// happen on the thread driving run()/run_one(); other threads may only post
// work via schedule() (e.g. cancellation), which wakes the loop through an
// eventfd.
//
// Unlike poll(), epoll keeps exactly one registration per file descriptor,
// so this backend multiplexes the (at most) two outstanding operations per
// socket - one waiting for readability, one for writability - in a per-fd
// record keyed by the native handle.  Events find the record through
// epoll_event.data.fd, making dispatch O(1) and immune to the socket_id
// reuse of the free-list container.
//
// Every socket registered with this context is put into non-blocking mode:
// work() is always attempted inline first and interest is only registered
// when the operation reports EWOULDBLOCK (submit_result::submit).

struct netexec::detail::epoll_context final : ::netexec::detail::context_base {
    struct epoll_record {
        ::std::uint32_t mask{}; // interest currently registered with epoll (0 = none)
        io_base*        in_op{};
        io_base*        out_op{};
    };

    using time_t       = ::std::chrono::system_clock::time_point;
    using timer_node_t = ::netexec::detail::context_base::resume_at_operation;
    struct get_time {
        auto operator()(auto* t) const -> time_t { return ::std::get<0>(*t); }
    };
    using timer_priority_t = ::netexec::detail::sorted_list<timer_node_t, ::std::less<>, get_time>;

    int                                                                             d_epoll_fd{-1};
    int                                                                             d_wakeup_fd{-1};
    ::netexec::detail::container<::netexec::detail::native_handle_type>             d_sockets;
    ::std::unordered_map<::netexec::detail::native_handle_type, epoll_record>       d_records;
    timer_priority_t                                                                d_timeouts;
    ::netexec::detail::context_base::task*                                          d_tasks{};
    ::std::mutex                                                                    d_tasks_mutex;
    ::std::atomic<::std::size_t>                                                    d_pending{}; // ops in record slots

    epoll_context() {
        this->d_epoll_fd = ::epoll_create1(EPOLL_CLOEXEC);
        if (this->d_epoll_fd < 0) {
            throw ::std::system_error(errno, ::std::system_category(), "epoll_create1 failed");
        }
        this->d_wakeup_fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (this->d_wakeup_fd < 0) {
            int error(errno);
            ::close(this->d_epoll_fd);
            throw ::std::system_error(error, ::std::system_category(), "eventfd failed");
        }
        ::epoll_event ev{};
        ev.events  = EPOLLIN;
        ev.data.fd = this->d_wakeup_fd;
        if (::epoll_ctl(this->d_epoll_fd, EPOLL_CTL_ADD, this->d_wakeup_fd, &ev) < 0) {
            int error(errno);
            ::close(this->d_wakeup_fd);
            ::close(this->d_epoll_fd);
            throw ::std::system_error(error, ::std::system_category(), "epoll_ctl(ADD) failed");
        }
    }
    ~epoll_context() override {
        if (0 <= this->d_wakeup_fd) {
            ::close(this->d_wakeup_fd);
        }
        if (0 <= this->d_epoll_fd) {
            ::close(this->d_epoll_fd);
        }
    }

    auto make_socket(::netexec::detail::native_handle_type handle) -> ::netexec::detail::socket_id override final {
        // Every fd driven by this backend is made non-blocking: a blocking
        // syscall would park the entire event loop.  This also covers sockets
        // produced by accept() (POSIX does not inherit O_NONBLOCK).
        ::fcntl(handle, F_SETFL, O_NONBLOCK);
        this->d_records.emplace(handle, epoll_record{});
        return this->d_sockets.insert(handle);
    }
    auto make_socket(int d, int t, int p, ::std::error_code& error) -> ::netexec::detail::socket_id override final {
        int fd(::socket(d, t, p));
        if (fd < 0) {
            error = ::std::error_code(errno, ::std::system_category());
            return ::netexec::detail::socket_id::invalid;
        }
        return this->make_socket(static_cast<::netexec::detail::native_handle_type>(fd));
    }
    auto release(::netexec::detail::socket_id id, ::std::error_code& error) -> void override final {
        // Closing the fd automatically drops it from the epoll interest list.
        // As with the other backends, a socket must not be released while
        // operations are outstanding on it.
        ::netexec::detail::native_handle_type handle(this->d_sockets[id]);
        this->d_sockets.erase(id);
        this->d_records.erase(handle);
        if (::close(handle) < 0) {
            error = ::std::error_code(errno, ::std::system_category());
        }
    }
    auto native_handle(::netexec::detail::socket_id id) -> ::netexec::detail::native_handle_type override final {
        return this->d_sockets[id];
    }
    auto set_option(::netexec::detail::socket_id id,
                    int                             level,
                    int                             name,
                    const void*                     data,
                    ::socklen_t                     size,
                    ::std::error_code&              error) -> void override final {
        if (::setsockopt(this->native_handle(id), level, name, data, size) < 0) {
            error = ::std::error_code(errno, ::std::system_category());
        }
    }
    auto bind(::netexec::detail::socket_id       id,
              const ::netexec::detail::endpoint& endpoint,
              ::std::error_code&                 error) -> void override final {
        if (::bind(this->native_handle(id), endpoint.data(), endpoint.size()) < 0) {
            error = ::std::error_code(errno, ::std::system_category());
        }
    }
    auto listen(::netexec::detail::socket_id id, int no, ::std::error_code& error) -> void override final {
        if (::listen(this->native_handle(id), no) < 0) {
            error = ::std::error_code(errno, ::std::system_category());
        }
    }

    // ---- event loop helpers -------------------------------------------------

    auto process_task() -> ::std::size_t {
        ::netexec::detail::context_base::task* tsk;
        {
            ::std::lock_guard lock(this->d_tasks_mutex);
            tsk = this->d_tasks;
            if (tsk != nullptr) {
                this->d_tasks = tsk->next;
            }
        }
        if (tsk != nullptr) {
            tsk->complete();
            return 1u;
        }
        return 0u;
    }
    auto process_timeout(const auto& now) -> ::std::size_t {
        if (!this->d_timeouts.empty() && ::std::get<0>(*this->d_timeouts.front()) <= now) {
            this->d_timeouts.pop_front()->complete();
            return 1u;
        }
        return 0u;
    }
    auto to_milliseconds(auto duration) -> int {
        return int(::std::chrono::duration_cast<::std::chrono::milliseconds>(duration).count());
    }
    auto drain_wakeup() -> void {
        ::std::uint64_t value{};
        while (::read(this->d_wakeup_fd, &value, sizeof(value)) == sizeof(value)) {
        }
    }
    auto wakeup() -> void {
        const ::std::uint64_t one = 1;
        // Failure (e.g. EAGAIN on a full counter) means the waiting thread is
        // already being woken up; nothing more to do.
        (void)::write(this->d_wakeup_fd, &one, sizeof(one));
    }
    auto wake() -> void override final { this->wakeup(); }

    // Reconcile the kernel interest for one fd with its record.  Only called
    // on the event loop thread.
    auto update_interest(::netexec::detail::native_handle_type handle, epoll_record& rec) -> ::std::error_code {
        ::std::uint32_t want((rec.in_op ? ::std::uint32_t(EPOLLIN) : 0u) | (rec.out_op ? ::std::uint32_t(EPOLLOUT) : 0u));
        if (want == rec.mask) {
            return {};
        }
        ::epoll_event ev{};
        ev.events  = want;
        ev.data.fd = handle;
        int ctl(rec.mask == 0 ? EPOLL_CTL_ADD : (want == 0 ? EPOLL_CTL_DEL : EPOLL_CTL_MOD));
        if (::epoll_ctl(this->d_epoll_fd, ctl, handle, want == 0 ? nullptr : &ev) < 0) {
            return {errno, ::std::system_category()};
        }
        rec.mask = want;
        return {};
    }

    // Run one ready operation from the given slot.  The operation is taken
    // out of the slot (and interest is updated) *before* work() runs: work()
    // may complete the operation and re-enter the context from the
    // continuation, including releasing this socket and erasing the record.
    // When work() reports submit (EWOULDBLOCK) no completion fired, hence no
    // user code ran and the record is guaranteed to be alive with the slot
    // still empty.
    auto dispatch(io_base*& slot, ::netexec::detail::native_handle_type handle, epoll_record& rec) -> ::std::size_t {
        io_base* op(slot);
        if (op == nullptr) {
            return 0u;
        }
        slot = nullptr;
        --this->d_pending;
        (void)this->update_interest(handle, rec);
        if (op->work(*this, op) == ::netexec::detail::submit_result::submit) {
            slot = op;
            ++this->d_pending;
            (void)this->update_interest(handle, rec);
        }
        return 1u;
    }

    auto add_outstanding(io_base* completion, bool try_inline = true) -> ::netexec::detail::submit_result {
        auto handle(this->d_sockets[completion->id]);
        auto it(this->d_records.find(handle));
        assert(it != this->d_records.end());
        epoll_record& rec(it->second);
        if (!try_inline || completion->work(*this, completion) == ::netexec::detail::submit_result::submit) {
            io_base*& slot(bool(completion->event & ::netexec::event_type::out) ? rec.out_op : rec.in_op);
            if (slot != nullptr) {
                // At most one outstanding operation per direction per socket.
                completion->error(::std::make_error_code(::std::errc::device_or_resource_busy));
                return ::netexec::detail::submit_result::error;
            }
            slot = completion;
            ++this->d_pending;
            if (auto ec = this->update_interest(handle, rec)) {
                slot = nullptr;
                --this->d_pending;
                completion->error(ec);
                return ::netexec::detail::submit_result::error;
            }
            this->wakeup();
            return ::netexec::detail::submit_result::submit;
        }
        return ::netexec::detail::submit_result::ready;
    }

    auto run_one() -> ::std::size_t override final {
        auto now(::std::chrono::system_clock::now());
        if (0u < this->process_timeout(now) || 0u < this->process_task()) {
            return 1u;
        }
        // The wake-up fd is registered permanently; "no work" means no
        // outstanding operation, no timer and no work guard.
        if (this->d_pending.load() == 0u && this->d_timeouts.empty() && this->work_count.load() == 0u) {
            return ::std::size_t{};
        }
        ::epoll_event events[16];
        while (true) {
            now              = ::std::chrono::system_clock::now();
            auto next_time{this->d_timeouts.value_or(now)};
            int  timeout{now == next_time ? -1 : this->to_milliseconds(next_time - now)};
            int  n(::epoll_wait(this->d_epoll_fd, events, int(sizeof(events) / sizeof(*events)), timeout));
            if (n < 0) {
                switch (errno) {
                default:
                    return ::std::size_t{};
                case EINTR:
                    break;
                }
            } else {
                for (int i(0); i < n; ++i) {
                    auto& ev(events[i]);
                    if (ev.data.fd == this->d_wakeup_fd) {
                        // Work was posted from another thread: drain the
                        // signal and keep scanning the batch; the tasks are
                        // picked up right after the event loop below so real
                        // events sharing the batch are not delayed a cycle.
                        this->drain_wakeup();
                        continue;
                    }
                    auto it(this->d_records.find(ev.data.fd));
                    if (it == this->d_records.end()) {
                        continue; // stale event for an fd closed after the snapshot
                    }
                    ::std::size_t done{};
                    if (ev.events & (EPOLLIN | EPOLLHUP | EPOLLERR)) {
                        done += this->dispatch(it->second.in_op, ev.data.fd, it->second);
                    }
                    if (ev.events & (EPOLLOUT | EPOLLHUP | EPOLLERR)) {
                        if (0u < done) {
                            // The in-dispatch ran a completion whose
                            // continuation may have released this socket and
                            // erased the record; revalidate before use.
                            it = this->d_records.find(ev.data.fd);
                            if (it == this->d_records.end()) {
                                return done;
                            }
                        }
                        done += this->dispatch(it->second.out_op, ev.data.fd, it->second);
                    }
                    if (0u < done) {
                        return done;
                    }
                }
            }
            // Whenever epoll_wait() wakes (or fails) without dispatching a
            // completion, check for work posted from other threads - e.g.
            // cancellation tasks - and for expired timers.
            if (0u < this->process_task()) {
                return 1u;
            }
            if (0u < this->process_timeout(::std::chrono::system_clock::now())) {
                return 1u;
            }
        }
    }

    auto cancel_on_loop(::netexec::detail::io_base* cancel_op, ::netexec::detail::io_base* op)
        -> void override final {
        // Timers and other non-socket operations carry no valid socket id, so
        // the operation cannot be located through it; scan the records
        // instead (cancellation is a cold path).
        for (auto& entry : this->d_records) {
            io_base** slot(entry.second.in_op == op    ? &entry.second.in_op
                           : entry.second.out_op == op ? &entry.second.out_op
                                                       : nullptr);
            if (slot != nullptr) {
                *slot = nullptr;
                --this->d_pending;
                (void)this->update_interest(entry.first, entry.second);
                op->cancel();
                cancel_op->cancel();
                return;
            }
        }
        if (this->d_timeouts.erase(op)) {
            op->cancel();
        }
        cancel_op->cancel();
    }
    auto schedule(::netexec::detail::context_base::task* tsk) -> void override final {
        {
            ::std::lock_guard lock(this->d_tasks_mutex);
            tsk->next     = this->d_tasks;
            this->d_tasks = tsk;
        }
        this->wakeup();
    }

    // ---- operations ----------------------------------------------------------

    auto poll(::netexec::detail::context_base::poll_operation* op)
        -> ::netexec::detail::submit_result override final {
        op->context = this;
        op->work    = [](::netexec::detail::context_base&, ::netexec::detail::io_base* o) {
            auto& cmp(*static_cast<poll_operation*>(o));
            cmp.complete();
            return ::netexec::detail::submit_result::ready;
        };
        return this->add_outstanding(op);
    }
    auto accept(::netexec::detail::context_base::accept_operation* completion)
        -> ::netexec::detail::submit_result override final {
        completion->context = this;
        completion->work    = [](::netexec::detail::context_base& ctxt, ::netexec::detail::io_base* comp) {
            auto  id{comp->id};
            auto& cmp(*static_cast<accept_operation*>(comp));
            while (true) {
                // The listen socket is non-blocking (see make_socket), so an
                // empty accept queue - e.g. a connection reset between the
                // readiness notification and this call - reports EWOULDBLOCK
                // instead of parking the event loop.
                int rc = ::accept(ctxt.native_handle(id), ::std::get<0>(cmp).data(), &::std::get<1>(cmp));
                if (0 <= rc) {
                    ::std::get<2>(cmp) = ctxt.make_socket(static_cast<::netexec::detail::native_handle_type>(rc));
                    cmp.complete();
                    return ::netexec::detail::submit_result::ready;
                }
                switch (errno) {
                default:
                    cmp.error(::std::error_code(errno, ::std::system_category()));
                    return ::netexec::detail::submit_result::error;
                case EINTR:
                    break;
                case EWOULDBLOCK:
                    return ::netexec::detail::submit_result::submit;
                }
            }
        };
        return this->add_outstanding(completion);
    }
    auto connect(::netexec::detail::context_base::connect_operation* op)
        -> ::netexec::detail::submit_result override {
        op->context = this;
        // The socket is already non-blocking (see make_socket): connect()
        // reports EINPROGRESS rather than blocking.
        auto        handle{this->native_handle(op->id)};
        const auto& endpoint(::std::get<0>(*op));
        if (0 == ::connect(handle, endpoint.data(), endpoint.size())) {
            op->complete();
            return ::netexec::detail::submit_result::ready;
        }
        switch (errno) {
        default:
            op->error(::std::error_code(errno, ::std::system_category()));
            return ::netexec::detail::submit_result::error;
        case EINPROGRESS:
        case EINTR:
            break;
        }

        op->work = [](::netexec::detail::context_base& ctxt, ::netexec::detail::io_base* o) {
            auto        hndl{ctxt.native_handle(o->id)};
            int         error{};
            ::socklen_t len{sizeof(error)};
            if (-1 == ::getsockopt(hndl, SOL_SOCKET, SO_ERROR, &error, &len)) {
                o->error(::std::error_code(errno, ::std::system_category()));
                return ::netexec::detail::submit_result::error;
            }
            if (0 == error) {
                o->complete();
                return ::netexec::detail::submit_result::ready;
            }
            o->error(::std::error_code(error, ::std::system_category()));
            return ::netexec::detail::submit_result::error;
        };

        // Register without trying work() inline: a non-blocking connect in
        // progress reports SO_ERROR == 0 and would be mistaken for a
        // completed connection.
        return this->add_outstanding(op, false);
    }
    auto receive(::netexec::detail::context_base::receive_operation* op)
        -> ::netexec::detail::submit_result override {
        op->context = this;
        op->work    = [](::netexec::detail::context_base& ctxt, ::netexec::detail::io_base* o) {
            auto& completion(*static_cast<receive_operation*>(o));
            while (true) {
                auto rc{::recvmsg(ctxt.native_handle(o->id), &::std::get<0>(completion), ::std::get<1>(completion))};
                if (0 <= rc) {
                    ::std::get<2>(completion) = ::std::size_t(rc);
                    completion.complete();
                    return ::netexec::detail::submit_result::ready;
                }
                switch (errno) {
                default:
                    completion.error(::std::error_code(errno, ::std::system_category()));
                    return ::netexec::detail::submit_result::error;
                case EINTR:
                    break;
                case EWOULDBLOCK:
                    return ::netexec::detail::submit_result::submit;
                }
            }
        };
        return this->add_outstanding(op);
    }
    auto send(::netexec::detail::context_base::send_operation* op) -> ::netexec::detail::submit_result override {
        op->context = this;
        op->work    = [](::netexec::detail::context_base& ctxt, ::netexec::detail::io_base* o) {
            auto& completion(*static_cast<send_operation*>(o));
            while (true) {
                auto rc{::sendmsg(ctxt.native_handle(o->id), &::std::get<0>(completion), ::std::get<1>(completion))};
                if (0 <= rc) {
                    ::std::get<2>(completion) = ::std::size_t(rc);
                    completion.complete();
                    return ::netexec::detail::submit_result::ready;
                }
                switch (errno) {
                default:
                    completion.error(::std::error_code(errno, ::std::system_category()));
                    return ::netexec::detail::submit_result::error;
                case EINTR:
                    break;
                case EWOULDBLOCK:
                    return ::netexec::detail::submit_result::submit;
                }
            }
        };
        return this->add_outstanding(op);
    }
    auto resume_at(::netexec::detail::context_base::resume_at_operation* op)
        -> ::netexec::detail::submit_result override {
        if (::std::chrono::system_clock::now() < ::std::get<0>(*op)) {
            this->d_timeouts.insert(op);
            return ::netexec::detail::submit_result::submit;
        }
        op->complete();
        return ::netexec::detail::submit_result::ready;
    }
};

// ----------------------------------------------------------------------------

#endif // NET_EXEC_USE_EPOLL
#endif
