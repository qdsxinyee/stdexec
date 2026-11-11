// include/netexec/__detail/uring_context.hpp                   -*-C++-*-
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef INCLUDED_NETEXEC_DETAIL_URING_CONTEXT
#define INCLUDED_NETEXEC_DETAIL_URING_CONTEXT

#include <netexec/__detail/container.hpp>
#include <netexec/__detail/context_base.hpp>

#include <cassert>
#include <cstdint>
#include <mutex>
#include <system_error>
#include <tuple>
#include <liburing.h>
#include <sys/eventfd.h>

namespace netexec::detail {

// io_context implementation based on liburing
struct uring_context final : context_base {
    struct wakeup_operation final : io_base {
        ::std::uint64_t value{};
        wakeup_operation() : io_base(::netexec::detail::socket_id::invalid, ::netexec::event_type::none) {}
        auto complete() -> void override {}
        auto error(::std::error_code) -> void override {}
        auto cancel() -> void override {}
    };

    static constexpr unsigned     QUEUE_DEPTH = 128;
    ::io_uring                    ring;
    container<native_handle_type> sockets;
    task*                         tasks = nullptr;
    ::std::mutex                  tasks_mutex;
    ::std::size_t                 submitting  = 0; // sqes not yet submitted
    ::std::size_t                 outstanding = 0; // user cqes expected
    int                           wakeup_fd{-1};
    wakeup_operation              wakeup;

    uring_context() {
        int flags = 0;
        int r     = ::io_uring_queue_init(QUEUE_DEPTH, &ring, flags);
        if (r < 0) {
            throw ::std::system_error(-r, ::std::system_category(), "io_uring_queue_init failed");
        }
        this->wakeup_fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (this->wakeup_fd < 0) {
            int error = errno;
            ::io_uring_queue_exit(&ring);
            throw ::std::system_error(error, ::std::system_category(), "eventfd failed");
        }
        this->wakeup.work = [](context_base& ctx, io_base* io) {
            auto& self = static_cast<uring_context&>(ctx);
            int   res  = *static_cast<int*>(io->extra.get());
            if (res < 0 && res != -EINTR) {
                throw ::std::system_error(-res, ::std::system_category(), "eventfd read failed");
            }
            self.arm_wakeup();
            return submit_result::submit;
        };
        this->arm_wakeup();
    }
    ~uring_context() override {
        ::io_uring_queue_exit(&ring);
        ::close(this->wakeup_fd);
    }

    auto make_socket(native_handle_type handle) -> socket_id override { return sockets.insert(handle); }

    auto make_socket(int d, int t, int p, ::std::error_code& error) -> socket_id override {
        int fd(::socket(d, t, p));
        if (fd < 0) {
            error = ::std::error_code(errno, ::std::system_category());
            return socket_id::invalid;
        }
        return make_socket(static_cast<native_handle_type>(fd));
    }

    auto release(socket_id id, ::std::error_code& error) -> void override {
        const native_handle_type handle = sockets[id];
        sockets.erase(id);
        if (::close(handle) < 0) {
            error = ::std::error_code(errno, ::std::system_category());
        }
    }

    auto native_handle(socket_id id) -> native_handle_type override { return sockets[id]; }

    auto set_option(socket_id id, int level, int name, const void* data, ::socklen_t size, ::std::error_code& error)
        -> void override {
        if (::setsockopt(native_handle(id), level, name, data, size) < 0) {
            error = ::std::error_code(errno, ::std::system_category());
        }
    }

    auto bind(socket_id id, const endpoint& ep, ::std::error_code& error) -> void override {
        if (::bind(native_handle(id), ep.data(), ep.size()) < 0) {
            error = ::std::error_code(errno, ::std::system_category());
        }
    }

    auto listen(socket_id id, int no, ::std::error_code& error) -> void override {
        if (::listen(native_handle(id), no) < 0) {
            error = ::std::error_code(errno, ::std::system_category());
        }
    }

    auto submit() -> void {
        int r = ::io_uring_submit(&ring);
        if (r < 0) {
            throw ::std::system_error(-r, ::std::system_category(), "io_uring_submit failed");
        }
        assert(submitting >= r);
        submitting -= r;
    }

    auto arm_wakeup() -> void {
        auto sqe = this->get_sqe(&this->wakeup);
        ::io_uring_prep_read(sqe, this->wakeup_fd, &this->wakeup.value, sizeof(this->wakeup.value), 0);
    }

    auto get_sqe(io_base* completion) -> ::io_uring_sqe* {
        auto sqe = ::io_uring_get_sqe(&ring);
        while (sqe == nullptr) {
            // if the submission queue is full, flush and try again
            submit();
            sqe = ::io_uring_get_sqe(&ring);
        }
        ::io_uring_sqe_set_data(sqe, completion);
        ++submitting;
        if (completion != &this->wakeup) {
            ++outstanding;
        }
        return sqe;
    }

    auto wait() -> ::std::tuple<int, io_base*> {
        ::io_uring_cqe* cqe = nullptr;
        int             r;
        do {
            r = ::io_uring_wait_cqe(&ring, &cqe);
        } while (r == -EINTR);
        if (r < 0) {
            throw ::std::system_error(-r, ::std::system_category(), "io_uring_wait_cqe failed");
        }

        const int  res        = cqe->res;
        const auto completion = static_cast<io_base*>(::io_uring_cqe_get_data(cqe));
        ::io_uring_cqe_seen(&ring, cqe);
        if (completion != &this->wakeup) {
            assert(outstanding > 0);
            --outstanding;
        }

        return {res, completion};
    }

    auto run_one() -> ::std::size_t override {
        if (auto count = process_task(); count) {
            return count;
        }

        if (!outstanding && this->work_count.load() == 0u) {
            return 0;
        }

        if (submitting) {
            // if we have anything to submit, batch the submit and wait in a
            // single system call. this allows io_uring_wait_cqe() below to be
            // served directly from memory
            unsigned wait_nr = 1;
            int      r;
            do {
                r = ::io_uring_submit_and_wait(&ring, wait_nr);
            } while (r == -EINTR);
            if (r < 0) {
                throw ::std::system_error(-r, ::std::system_category(), "io_uring_submit_and_wait failed");
            }
            assert(submitting >= static_cast<::std::size_t>(r));
            submitting -= r;
        }

        auto [res, completion] = wait();
        completion->extra.reset(&res);
        completion->work(*this, completion);

        return 1;
    }

    auto cancel_on_loop(io_base* cancel_op, io_base* op) -> void override {
        while (submitting) {
            submit();
        }
        cancel_op->work = [](context_base&, io_base* io) {
            auto res = *static_cast<int*>(io->extra.get());
            if (res == -ENOENT || res == -EALREADY) { // op already completed
                io->cancel();
                return submit_result::ready;
            } else if (res < 0) {
                io->error(::std::error_code(-res, ::std::system_category()));
                return submit_result::error;
            }
            io->complete();
            return submit_result::ready;
        };

        auto sqe   = get_sqe(cancel_op);
        int  flags = 0;
        ::io_uring_prep_cancel(sqe, op, flags);
    }

    auto schedule(task* t) -> void override {
        {
            ::std::lock_guard lock(this->tasks_mutex);
            t->next = tasks;
            tasks   = t;
        }
        this->wake();
    }

    auto wake() -> void override {
        const ::std::uint64_t one = 1;
        (void)::write(this->wakeup_fd, &one, sizeof(one));
    }

    auto poll(poll_operation* op) -> submit_result override {
        op->error(::std::make_error_code(::std::errc::operation_not_supported));
        return submit_result::error;
    }

    auto process_task() -> ::std::size_t {
        task* t{};
        {
            ::std::lock_guard lock(this->tasks_mutex);
            if (tasks) {
                t     = tasks;
                tasks = t->next;
            }
        }
        if (t) {
            t->complete();
            return 1u;
        }
        return 0u;
    }

    auto accept(accept_operation* op) -> submit_result override {
        op->work = [](context_base& ctx, io_base* io) {
            auto res = *static_cast<int*>(io->extra.get());
            if (res == -ECANCELED) {
                io->cancel();
                return submit_result::ready;
            } else if (res < 0) {
                io->error(::std::error_code(-res, ::std::system_category()));
                return submit_result::error;
            }
            auto op = static_cast<accept_operation*>(io);
            // set socket
            ::std::get<2>(*op) = ctx.make_socket(res);
            io->complete();
            return submit_result::ready;
        };

        auto sqe     = get_sqe(op);
        auto fd      = native_handle(op->id);
        auto addr    = ::std::get<0>(*op).data();
        auto addrlen = &::std::get<1>(*op);
        int  flags   = 0;
        ::io_uring_prep_accept(sqe, fd, addr, addrlen, flags);
        return submit_result::submit;
    }

    auto connect(connect_operation* op) -> submit_result override {
        op->work = [](context_base&, io_base* io) {
            auto res = *static_cast<int*>(io->extra.get());
            if (res == -ECANCELED) {
                io->cancel();
                return submit_result::ready;
            } else if (res < 0) {
                io->error(::std::error_code(-res, ::std::system_category()));
                return submit_result::error;
            }
            io->complete();
            return submit_result::ready;
        };

        auto  sqe  = get_sqe(op);
        auto  fd   = native_handle(op->id);
        auto& addr = ::std::get<0>(*op);
        ::io_uring_prep_connect(sqe, fd, addr.data(), addr.size());
        return submit_result::submit;
    }

    auto receive(receive_operation* op) -> submit_result override {
        op->work = [](context_base&, io_base* io) {
            auto res = *static_cast<int*>(io->extra.get());
            if (res == -ECANCELED) {
                io->cancel();
                return submit_result::ready;
            } else if (res < 0) {
                io->error(::std::error_code(-res, ::std::system_category()));
                return submit_result::error;
            }
            auto op = static_cast<receive_operation*>(io);
            // set bytes received
            ::std::get<2>(*op) = res;
            io->complete();
            return submit_result::ready;
        };

        auto sqe   = get_sqe(op);
        auto fd    = native_handle(op->id);
        auto msg   = &::std::get<0>(*op);
        auto flags = ::std::get<1>(*op);
        ::io_uring_prep_recvmsg(sqe, fd, msg, flags);
        return submit_result::submit;
    }

    auto send(send_operation* op) -> submit_result override {
        op->work = [](context_base&, io_base* io) {
            auto res = *static_cast<int*>(io->extra.get());
            if (res == -ECANCELED) {
                io->cancel();
                return submit_result::ready;
            } else if (res < 0) {
                io->error(::std::error_code(-res, ::std::system_category()));
                return submit_result::error;
            }
            auto op = static_cast<send_operation*>(io);
            // set bytes sent
            ::std::get<2>(*op) = res;
            io->complete();
            return submit_result::ready;
        };

        auto sqe   = get_sqe(op);
        auto fd    = native_handle(op->id);
        auto msg   = &::std::get<0>(*op);
        auto flags = ::std::get<1>(*op);
        ::io_uring_prep_sendmsg(sqe, fd, msg, flags);
        return submit_result::submit;
    }

    static auto make_timespec(auto dur) -> __kernel_timespec {
        auto sec = ::std::chrono::duration_cast<::std::chrono::seconds>(dur);
        dur -= sec;
        auto              nsec = ::std::chrono::duration_cast<::std::chrono::nanoseconds>(dur);
        __kernel_timespec ts;
        ts.tv_sec  = sec.count();
        ts.tv_nsec = nsec.count();
        return ts;
    }

    auto resume_at(resume_at_operation* op) -> submit_result override {
        auto at  = ::std::get<0>(*op);
        op->work = [](context_base&, io_base* io) {
            auto res = *static_cast<int*>(io->extra.get());
            auto op  = static_cast<resume_at_operation*>(io);
            if (res == -ECANCELED) {
                io->cancel();
                return submit_result::ready;
            } else if (res == -ETIME) {
                io->complete();
                return submit_result::ready;
            }
            io->error(::std::error_code(-res, ::std::system_category()));
            return submit_result::error;
        };

        auto     sqe = get_sqe(op);
        auto&    ts  = ::std::get<1>(*op);
        ts           = make_timespec(at.time_since_epoch());
        unsigned count = 0;
        unsigned flags = IORING_TIMEOUT_ABS | IORING_TIMEOUT_REALTIME;
        ::io_uring_prep_timeout(sqe, &ts, count, flags);
        return submit_result::submit;
    }
};

} // namespace netexec::detail

#endif
