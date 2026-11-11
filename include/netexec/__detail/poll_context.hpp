// include/netexec/__detail/poll_context.hpp                    -*-C++-*-
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef INCLUDED_NETEXEC_DETAIL_POLL_CONTEXT
#define INCLUDED_NETEXEC_DETAIL_POLL_CONTEXT

// ----------------------------------------------------------------------------

#include <netexec/__detail/platform.hpp>
#include <netexec/__detail/netfwd.hpp>
#include <netexec/__detail/container.hpp>
#include <netexec/__detail/context_base.hpp>
#include <netexec/__detail/sorted_list.hpp>
#if !defined(_MSC_VER)
#  include <sys/eventfd.h>
#endif
#include <cstdint>
#include <mutex>
#include <vector>
#include <algorithm>
#include <iostream>

// ----------------------------------------------------------------------------

namespace netexec::detail {
struct poll_record;
struct poll_context;
} // namespace netexec::detail

// ----------------------------------------------------------------------------

struct netexec::detail::poll_record final {
    poll_record(::netexec::detail::native_handle_type h) : handle(h) {}
    ::netexec::detail::native_handle_type handle;
    bool                                     blocking{true};
};

// ----------------------------------------------------------------------------

struct netexec::detail::poll_context final : ::netexec::detail::context_base {
#ifdef _MSC_VER
    // On Windows, Winsock must be initialized before any socket call.
    // This RAII guard calls WSAStartup on construction and WSACleanup on
    // destruction, tying the Winsock lifetime to the poll_context object.
    struct wsa_guard {
        wsa_guard() {
            ::WSADATA wd{};
            if (::WSAStartup(MAKEWORD(2, 2), &wd) != 0)
                throw ::std::system_error(::WSAGetLastError(), ::std::system_category(), "WSAStartup failed");
        }
        ~wsa_guard() { ::WSACleanup(); }
        wsa_guard(const wsa_guard&)            = delete;
        wsa_guard& operator=(const wsa_guard&) = delete;
    } d_wsa; // constructed first, destroyed last
#endif

    using time_t       = ::std::chrono::system_clock::time_point;
    using timer_node_t = ::netexec::detail::context_base::resume_at_operation;
    struct get_time {
        auto operator()(auto* t) const -> time_t { return ::std::get<0>(*t); }
    };
    using timer_priority_t = ::netexec::detail::sorted_list<timer_node_t, ::std::less<>, get_time>;
    ::netexec::detail::container<::netexec::detail::poll_record> d_sockets;
    ::std::vector<::pollfd>                                            d_poll;
    ::std::vector<::netexec::detail::io_base*>                      d_outstanding;
    timer_priority_t                                                   d_timeouts;
    ::netexec::detail::context_base::task*                          d_tasks{};
    ::std::mutex                                                        d_tasks_mutex;
    // Wake-up channel used to interrupt poll() when work is posted from other
    // threads (e.g. the blocking thread pool, or a cancellation request).  It
    // is kept in d_poll as a permanent entry whose d_outstanding slot is a
    // nullptr sentinel.
#ifdef _MSC_VER
    // WSAPoll() can only wait on sockets, so a UDP socket connected to itself
    // acts as the wake-up channel (a poor man's eventfd).
    ::SOCKET d_wakeup_fd{INVALID_SOCKET};
#else
    int d_wakeup_fd{-1};
#endif

    poll_context() {
#ifdef _MSC_VER
        ::SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s == INVALID_SOCKET) {
            throw ::std::system_error(sock_errno(), ::std::system_category(), "wake-up socket failed");
        }
        ::sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
        addr.sin_port        = 0; // let the kernel pick a free port
        int addr_len         = sizeof(addr);
        if (::bind(s, reinterpret_cast<::sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR ||
            ::getsockname(s, reinterpret_cast<::sockaddr*>(&addr), &addr_len) == SOCKET_ERROR ||
            ::connect(s, reinterpret_cast<::sockaddr*>(&addr), addr_len) == SOCKET_ERROR) {
            int err = sock_errno();
            ::closesocket(s);
            throw ::std::system_error(err, ::std::system_category(), "wake-up socket setup failed");
        }
        ::u_long mode = 1;
        ::ioctlsocket(s, FIONBIO, &mode);
        this->d_wakeup_fd = s;
#else
        this->d_wakeup_fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (this->d_wakeup_fd < 0) {
            // Without a wake-up channel cross-thread wake-ups would silently
            // never arrive; fail the context construction instead.
            throw ::std::system_error(errno, ::std::system_category(), "eventfd failed");
        }
#endif
        this->d_poll.emplace_back(
            ::pollfd{static_cast<decltype(::pollfd{}.fd)>(this->d_wakeup_fd), static_cast<short>(POLLIN), short()});
        this->d_outstanding.emplace_back(nullptr);
    }
    ~poll_context() override {
#ifdef _MSC_VER
        if (this->d_wakeup_fd != INVALID_SOCKET) {
            ::closesocket(this->d_wakeup_fd);
        }
#else
        if (this->d_wakeup_fd >= 0) {
            ::close(this->d_wakeup_fd);
        }
#endif
    }

    auto drain_wakeup() -> void {
#ifdef _MSC_VER
        char buffer[64];
        while (0 < ::recv(this->d_wakeup_fd, buffer, sizeof(buffer), 0)) {
        }
#else
        ::std::uint64_t value{};
        while (::read(this->d_wakeup_fd, &value, sizeof(value)) == sizeof(value)) {
        }
#endif
    }

    auto make_socket(::netexec::detail::native_handle_type handle) -> ::netexec::detail::socket_id override final {
        return this->d_sockets.insert(handle);
    }
    auto make_socket(int d, int t, int p, ::std::error_code& error) -> ::netexec::detail::socket_id override final {
        // ::socket() returns SOCKET (ULONG_PTR) on Windows and int on POSIX.
        // INVALID_SOCKET / -1 are the respective failure sentinels.
#ifdef _MSC_VER
        ::SOCKET fd(::socket(d, t, p));
        if (fd == INVALID_SOCKET) {
            error = ::std::error_code(sock_errno(), ::std::system_category());
            return ::netexec::detail::socket_id::invalid;
        }
        return this->make_socket(static_cast<::netexec::detail::native_handle_type>(fd));
#else
        int fd(::socket(d, t, p));
        if (fd < 0) {
            error = ::std::error_code(sock_errno(), ::std::system_category());
            return ::netexec::detail::socket_id::invalid;
        }
        return this->make_socket(static_cast<::netexec::detail::native_handle_type>(fd));
#endif
    }

    // Mark a registered socket as non-blocking so that add_outstanding() will
    // call work() immediately instead of going straight to poll().
    // Must be called after make_socket() whenever the underlying fd has been
    // put into non-blocking mode outside of this class (e.g. in accept's work
    // lambda on Windows where F_GETFL is unavailable).
    auto set_nonblocking(::netexec::detail::socket_id id) -> void { this->d_sockets[id].blocking = false; }
    auto release(::netexec::detail::socket_id id, ::std::error_code& error) -> void override final {
        ::netexec::detail::native_handle_type handle(this->d_sockets[id].handle);
        this->d_sockets.erase(id);
        if (::close(handle) < 0) {
            error = ::std::error_code(sock_errno(), ::std::system_category());
        }
    }
    auto native_handle(::netexec::detail::socket_id id) -> ::netexec::detail::native_handle_type override final {
        return this->d_sockets[id].handle;
    }
    auto set_option(::netexec::detail::socket_id id,
                    int                             level,
                    int                             name,
                    const void*                     data,
                    ::socklen_t                     size,
                    ::std::error_code&              error) -> void override final {
        if (::setsockopt(this->native_handle(id), level, name, data, size) < 0) {
            error = ::std::error_code(sock_errno(), ::std::system_category());
        }
    }
    auto bind(::netexec::detail::socket_id       id,
              const ::netexec::detail::endpoint& endpoint,
              ::std::error_code&                    error) -> void override final {
        if (::bind(this->native_handle(id), endpoint.data(), endpoint.size()) < 0) {
            error = ::std::error_code(sock_errno(), ::std::system_category());
        }
    }
    auto listen(::netexec::detail::socket_id id, int no, ::std::error_code& error) -> void override final {
        if (::listen(this->native_handle(id), no) < 0) {
            error = ::std::error_code(sock_errno(), ::std::system_category());
        }
    }

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
    auto remove_outstanding(::std::size_t i) {
        if (i + 1u != this->d_poll.size()) {
            this->d_poll[i]        = this->d_poll.back();
            this->d_outstanding[i] = this->d_outstanding.back();
        }
        this->d_poll.pop_back();
        this->d_outstanding.pop_back();
    }
    auto to_milliseconds(auto duration) -> int {
        return int(::std::chrono::duration_cast<::std::chrono::milliseconds>(duration).count());
    }
    auto run_one() noexcept -> ::std::size_t override final {
        auto now{::std::chrono::system_clock::now()};
        if (0u < this->process_timeout(now) || 0 < this->process_task()) {
            return 1u;
        }
        // The wake-up fd keeps a permanent entry with a nullptr outstanding
        // slot; "no work" means no real sockets left.
        const bool have_sockets =
            ::std::any_of(this->d_outstanding.begin(), this->d_outstanding.end(), [](auto* o) {
                return o != nullptr;
            });
        if (!have_sockets && this->d_timeouts.empty() && this->work_count.load() == 0u) {
            return ::std::size_t{};
        }
        while (true) {
            auto   next_time{this->d_timeouts.value_or(now)};
            int    timeout{now == next_time ? -1 : this->to_milliseconds(next_time - now)};
            if (timeout == -1 && this->work_count.load() > 0u) {
                timeout = 100;
            }
            nfds_t sz([](auto s) {
                if constexpr (::std::same_as<decltype(s), nfds_t>)
                    return s;
                else
                    return nfds_t(s);
            }(this->d_poll.size()));
            int    rc(::poll(this->d_poll.data(), sz, timeout));
            if (rc < 0) {
                // sock_errno() maps to WSAGetLastError() on Windows, errno on POSIX.
                // EINTR / EAGAIN macros are mapped to their WSA equivalents in platform.hpp.
                switch (sock_errno()) {
                default:
                    return ::std::size_t();
                case EINTR:
                case EAGAIN:
                    break;
                }
            } else {
                for (::std::size_t i(this->d_poll.size()); 0 < i--;) {
                    // Always dispatch on error/hang-up, not just on the requested
                    // events: on Windows WSAPoll reports a graceful peer close as
                    // POLLHUP *without* POLLIN, and Linux may report POLLERR only.
                    // The work functions turn these into EOF (0 bytes) or an error.
                    if (this->d_poll[i].revents & (this->d_poll[i].events | POLLERR | POLLHUP | POLLNVAL)) {
                        ::netexec::detail::io_base* completion = this->d_outstanding[i];
                        if (completion == nullptr) {
                            // Wake-up channel: drain it and re-enter run_one() so
                            // the tasks posted from other threads get processed.
                            this->drain_wakeup();
                            return ::std::size_t(1);
                        }
                        this->remove_outstanding(i);
                        if (completion->work(*this, completion) == ::netexec::detail::submit_result::submit) {
                            // The readiness indication was stale - e.g. an
                            // accept queue drained by a reset between poll()
                            // and accept(), or data consumed by a sibling
                            // operation on the same fd: re-register the
                            // operation instead of dropping it.
                            this->add_outstanding(completion);
                        }
                        return ::std::size_t(1);
                    }
                }
            }
            // Whenever poll() wakes (or is skipped) without dispatching a
            // completion, check for work posted from other threads - e.g.
            // cancellation tasks - and for expired timers. Without this, tasks
            // posted while the loop is blocked would only be noticed if a
            // socket event or the wake-up fd happened to arrive.
            if (0u < this->process_task()) {
                return 1u;
            }
            if (0u < this->process_timeout(::std::chrono::system_clock::now())) {
                return 1u;
            }
        }
        return ::std::size_t{};
    }
    auto wakeup() -> void {
#ifdef _MSC_VER
        if (this->d_wakeup_fd != INVALID_SOCKET) {
            const char one = 1;
            // Failure (e.g. WSAEWOULDBLOCK on a full buffer) means the polling
            // thread is already being woken up; nothing more to do.
            (void)::send(this->d_wakeup_fd, &one, 1, 0);
        }
#else
        if (this->d_wakeup_fd >= 0) {
            const ::std::uint64_t one = 1;
            // Failure (e.g. EAGAIN on a full counter) means the polling thread
            // is already being woken up; nothing more to do.
            (void)::write(this->d_wakeup_fd, &one, sizeof(one));
        }
#endif
    }
    auto wake() -> void override { this->wakeup(); }

    auto add_outstanding(::netexec::detail::io_base* completion) -> ::netexec::detail::submit_result {
        auto id{completion->id};
        if (this->d_sockets[id].blocking ||
            completion->work(*this, completion) == ::netexec::detail::submit_result::submit) {
            decltype(pollfd().events) events{};
            if (bool(completion->event & ::netexec::event_type::in)) {
                events |= POLLIN;
            }
            if (bool(completion->event & ::netexec::event_type::out)) {
                events |= POLLOUT;
            }
            // this->d_poll.emplace_back(::pollfd{this->native_handle(id), events, short()});
            //  pollfd::fd is int on POSIX and SOCKET (ULONG_PTR) on Windows.
            //  Cast through the actual field type to avoid narrowing warnings on
            //  both platforms without introducing a #ifdef here.
            this->d_poll.emplace_back(
                ::pollfd{static_cast<decltype(::pollfd{}.fd)>(this->native_handle(id)), events, short()});
            this->d_outstanding.emplace_back(completion);
            this->wakeup();
            return ::netexec::detail::submit_result::submit;
        }
        return ::netexec::detail::submit_result::ready;
    }

    auto cancel_on_loop(::netexec::detail::io_base* cancel_op, ::netexec::detail::io_base* op)
        -> void override final {
        auto it(::std::find(this->d_outstanding.begin(), this->d_outstanding.end(), op));
        if (it != this->d_outstanding.end()) {
            this->remove_outstanding(std::size_t(::std::distance(this->d_outstanding.begin(), it)));
            op->cancel();
        } else if (this->d_timeouts.erase(op)) {
            op->cancel();
        }
        cancel_op->cancel();
    }
    auto schedule(::netexec::detail::context_base::task* tsk) -> void override {
        {
            ::std::lock_guard lock(this->d_tasks_mutex);
            tsk->next     = this->d_tasks;
            this->d_tasks = tsk;
        }
        this->wakeup();
    }
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
        completion->work = [](::netexec::detail::context_base& ctxt, ::netexec::detail::io_base* comp) {
            auto  id{comp->id};
            auto& cmp(*static_cast<accept_operation*>(comp));

            while (true) {
#ifdef _MSC_VER
                // On Windows, ::accept() returns SOCKET (ULONG_PTR).
                ::SOCKET rc = ::accept(ctxt.native_handle(id), ::std::get<0>(cmp).data(), &::std::get<1>(cmp));
                if (rc == INVALID_SOCKET) {
                    switch (sock_errno()) {
                    default:
                        cmp.error(::std::error_code(sock_errno(), ::std::system_category()));
                        return ::netexec::detail::submit_result::error;
                    case WSAEINTR:
                        break; // retry
                    case WSAEWOULDBLOCK:
                        return ::netexec::detail::submit_result::submit;
                    }
                } else {
                    // Put the new socket into non-blocking mode so that subsequent
                    // async_receive / async_send calls can attempt work() immediately
                    // instead of blocking the event loop.
                    // platform.hpp shims fcntl(F_SETFL, O_NONBLOCK) to ioctlsocket(FIONBIO).
                    ::fcntl(static_cast<int>(rc), F_SETFL, O_NONBLOCK);
                    auto new_id = ctxt.make_socket(static_cast<::netexec::detail::native_handle_type>(rc));
                    // make_socket() cannot detect the non-blocking state on Windows
                    // (no F_GETFL equivalent), so we inform the context explicitly.
                    static_cast<poll_context&>(ctxt).set_nonblocking(new_id);
                    ::std::get<2>(cmp) = new_id;
                    cmp.complete();
                    return ::netexec::detail::submit_result::ready;
                }
#else
                int rc = ::accept(ctxt.native_handle(id), ::std::get<0>(cmp).data(), &::std::get<1>(cmp));
                if (0 <= rc) {
                    ::std::get<2>(cmp) = ctxt.make_socket(static_cast<::netexec::detail::native_handle_type>(rc));
                    cmp.complete();
                    return ::netexec::detail::submit_result::ready;
                } else {
                    switch (sock_errno()) {
                    default:
                        cmp.error(::std::error_code(sock_errno(), ::std::system_category()));
                        return ::netexec::detail::submit_result::error;
                    case EINTR:
                        break;
                    case EWOULDBLOCK:
                        return ::netexec::detail::submit_result::submit;
                    }
                }
#endif
            }
        };
        return this->add_outstanding(completion);
    }
    auto connect(::netexec::detail::context_base::connect_operation* op)
        -> ::netexec::detail::submit_result override {
        auto        handle{this->native_handle(op->id)};
        const auto& endpoint(::std::get<0>(*op));
        if (-1 == ::fcntl(handle, F_SETFL, O_NONBLOCK)) {
            op->error(::std::error_code(sock_errno(), ::std::system_category()));
            return ::netexec::detail::submit_result::error;
        }
        if (0 == ::connect(handle, endpoint.data(), endpoint.size())) {
            op->complete();
            return ::netexec::detail::submit_result::ready;
        }
        switch (sock_errno()) {
        default:
            op->error(::std::error_code(sock_errno(), ::std::system_category()));
            return ::netexec::detail::submit_result::error;
        case EINPROGRESS:
#ifdef _MSC_VER
        // On Windows a non-blocking connect in progress reports WSAEWOULDBLOCK,
        // not EINPROGRESS (the two are distinct Winsock error codes even though
        // platform.hpp maps EINPROGRESS -> WSAEINPROGRESS for other purposes).
        case WSAEWOULDBLOCK:
#endif
        case EINTR:
            break;
        }

        op->context = this;
        op->work    = [](::netexec::detail::context_base& ctxt, ::netexec::detail::io_base* o) {
            auto hndl{ctxt.native_handle(o->id)};

            int         error{};
            ::socklen_t len{sizeof(error)};
            if (-1 == ::getsockopt(hndl, SOL_SOCKET, SO_ERROR, &error, &len)) {
                o->error(::std::error_code(sock_errno(), ::std::system_category()));
                return ::netexec::detail::submit_result::error;
            }
            if (0 == error) {
                o->complete();
                return ::netexec::detail::submit_result::ready;
            } else {
                o->error(::std::error_code(error, ::std::system_category()));
                return ::netexec::detail::submit_result::error;
            }
        };

        // Register with poll() before marking the socket record non-blocking:
        // add_outstanding() attempts work() inline for non-blocking records, but
        // a non-blocking connect in progress reports SO_ERROR == 0 and would be
        // mistaken for a completed connection.
        auto result = this->add_outstanding(op);
#ifdef _MSC_VER
        // On Windows we must also update the blocking flag in the socket record
        // because F_GETFL is unavailable and make_socket() defaults to blocking=true.
        this->set_nonblocking(op->id);
#endif
        return result;
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
                } else
                    switch (sock_errno()) {
                    default:
                        completion.error(::std::error_code(sock_errno(), ::std::system_category()));
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
                } else
                    switch (sock_errno()) {
                    default:
                        completion.error(::std::error_code(sock_errno(), ::std::system_category()));
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
        } else {
            op->complete();
            return ::netexec::detail::submit_result::ready;
        }
    }
};

// ----------------------------------------------------------------------------

#endif
