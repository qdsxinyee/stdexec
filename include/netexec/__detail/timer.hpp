// include/beman/net/detail/timer.hpp                               -*-C++-*-
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef INCLUDED_BEMAN_NET_DETAIL_TIMER
#define INCLUDED_BEMAN_NET_DETAIL_TIMER

// ----------------------------------------------------------------------------

#include <netexec/__detail/netfwd.hpp>
#include <netexec/__detail/context_base.hpp>
#include <netexec/__detail/sender.hpp>
#include <netexec/__detail/event_type.hpp>

// ----------------------------------------------------------------------------

namespace netexec::detail {
struct resume_after_desc;
struct resume_at_desc;
} // namespace netexec::detail

namespace netexec {
using async_resume_after_t = ::netexec::detail::sender_cpo<::netexec::detail::resume_after_desc>;
using async_resume_at_t    = ::netexec::detail::sender_cpo<::netexec::detail::resume_at_desc>;

inline constexpr async_resume_after_t resume_after{};
inline constexpr async_resume_at_t    resume_at{};
} // namespace netexec

// ----------------------------------------------------------------------------

struct netexec::detail::resume_after_desc {
    using operation = ::netexec::detail::context_base::resume_after_operation;
    template <typename Scheduler, typename>
    struct data {
        using completion_signature = stdexec::set_value_t();

        ::std::remove_cvref_t<Scheduler> d_scheduler;
        ::std::chrono::microseconds      d_duration;

        auto id() const -> ::netexec::detail::socket_id { return {}; }
        auto events() const { return ::netexec::event_type::none; }
        auto get_scheduler() { return this->d_scheduler; }
        auto set_value(operation&, auto&& receiver) { stdexec::set_value(::std::move(receiver)); }
        auto submit(auto* base) -> ::netexec::detail::submit_result {
            ::std::get<0>(*base) = ::std::chrono::system_clock::now() + this->d_duration;
            return this->d_scheduler.resume_at(base);
        }
    };
};

// ----------------------------------------------------------------------------

struct netexec::detail::resume_at_desc {
    using operation = ::netexec::detail::context_base::resume_at_operation;
    template <typename Scheduler, typename>
    struct data {
        using completion_signature = stdexec::set_value_t();

        ::std::remove_cvref_t<Scheduler>        d_scheduler;
        ::std::chrono::system_clock::time_point d_time;

        auto id() const -> ::netexec::detail::socket_id { return {}; }
        auto events() const { return ::netexec::event_type::none; }
        auto get_scheduler() { return this->d_scheduler; }
        auto set_value(operation&, auto&& receiver) { stdexec::set_value(::std::move(receiver)); }
        auto submit(auto* base) -> ::netexec::detail::submit_result {
            ::std::get<0>(*base) = this->d_time;
            return this->d_scheduler.resume_at(base);
        }
    };
};

// ----------------------------------------------------------------------------

#endif
