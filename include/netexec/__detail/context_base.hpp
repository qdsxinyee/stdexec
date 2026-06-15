// include/beman/net/detail/context_base.hpp                        -*-C++-*-
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef INCLUDED_BEMAN_NET_DETAIL_CONTEXT_BASE
#define INCLUDED_BEMAN_NET_DETAIL_CONTEXT_BASE

#include <netexec/__detail/platform.hpp>
#include <netexec/__detail/io_base.hpp>
#include <netexec/__detail/endpoint.hpp>
#include <chrono>
#include <optional>
#include <system_error>
#include <type_traits>

// ----------------------------------------------------------------------------

namespace netexec::detail {
struct context_base;
}

// ----------------------------------------------------------------------------

struct netexec::detail::context_base {
    struct task {
        task* next;
        virtual ~task()                 = default;
        virtual auto complete() -> void = 0;
    };

    using accept_operation = ::netexec::detail::io_operation<
        ::std::tuple<::netexec::detail::endpoint, ::socklen_t, ::std::optional<::netexec::detail::socket_id>>>;
    using connect_operation = ::netexec::detail::io_operation<::std::tuple<::netexec::detail::endpoint>>;
    using receive_operation = ::netexec::detail::io_operation<::std::tuple<::msghdr, int, ::std::size_t>>;
    using send_operation    = ::netexec::detail::io_operation<::std::tuple<::msghdr, int, ::std::size_t>>;
    using resume_after_operation =
        ::netexec::detail::io_operation<::std::tuple<::std::chrono::system_clock::time_point, ::timeval>>;
    using resume_at_operation =
        ::netexec::detail::io_operation<::std::tuple<::std::chrono::system_clock::time_point, ::timeval>>;
    using poll_operation = ::netexec::detail::io_operation<::std::tuple<int, ::netexec::event_type>>;

    virtual ~context_base()                                                                                 = default;
    virtual auto make_socket(::netexec::detail::native_handle_type) -> ::netexec::detail::socket_id   = 0;
    virtual auto make_socket(int, int, int, ::std::error_code&) -> ::netexec::detail::socket_id          = 0;

    // Convenience overload for callers that still pass an int fd.
    // On POSIX native_handle_type is already int, so this overload is skipped
    // there to avoid colliding with make_socket(native_handle_type).
    template <typename = void>
        requires(!::std::is_same_v<::netexec::detail::native_handle_type, int>)
    auto make_socket(int fd) -> ::netexec::detail::socket_id {
        return this->make_socket(static_cast<::netexec::detail::native_handle_type>(fd));
    }

    virtual auto release(::netexec::detail::socket_id, ::std::error_code&) -> void                       = 0;
    virtual auto native_handle(::netexec::detail::socket_id) -> ::netexec::detail::native_handle_type = 0;
    virtual auto set_option(::netexec::detail::socket_id, int, int, const void*, ::socklen_t, ::std::error_code&)
        -> void = 0;
    virtual auto bind(::netexec::detail::socket_id, const ::netexec::detail::endpoint&, ::std::error_code&)
        -> void                                                                           = 0;
    virtual auto listen(::netexec::detail::socket_id, int, ::std::error_code&) -> void = 0;

    virtual auto run_one() noexcept -> ::std::size_t = 0;

    virtual auto cancel(::netexec::detail::io_base*, ::netexec::detail::io_base*) -> void = 0;
    virtual auto schedule(::netexec::detail::context_base::task*) -> void                    = 0;
    virtual auto poll(::netexec::detail::context_base::poll_operation*)
        -> ::netexec::detail::submit_result = 0;
    virtual auto accept(::netexec::detail::context_base::accept_operation*)
        -> ::netexec::detail::submit_result = 0;
    virtual auto connect(::netexec::detail::context_base::connect_operation*)
        -> ::netexec::detail::submit_result = 0;
    virtual auto receive(::netexec::detail::context_base::receive_operation*)
        -> ::netexec::detail::submit_result                                                                    = 0;
    virtual auto send(::netexec::detail::context_base::send_operation*) -> ::netexec::detail::submit_result = 0;
    virtual auto resume_at(::netexec::detail::context_base::resume_at_operation*)
        -> ::netexec::detail::submit_result = 0;
};

// ----------------------------------------------------------------------------

#endif
