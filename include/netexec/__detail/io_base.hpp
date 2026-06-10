// include/beman/net/detail/io_base.hpp                             -*-C++-*-
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef INCLUDED_BEMAN_NET_DETAIL_IO_BASE
#define INCLUDED_BEMAN_NET_DETAIL_IO_BASE

#include <netexec/__detail/netfwd.hpp>
#include <netexec/__detail/event_type.hpp>
#include <memory>
#include <system_error>
#include <ostream>

// ----------------------------------------------------------------------------

namespace netexec::detail {
enum class submit_result { ready, submit, error };
auto operator<<(::std::ostream&, ::netexec::detail::submit_result) -> ::std::ostream&;

struct io_base;
template <typename>
struct io_operation;
} // namespace netexec::detail

// ----------------------------------------------------------------------------

inline auto netexec::detail::operator<<(::std::ostream& out, ::netexec::detail::submit_result result)
    -> ::std::ostream& {
    switch (result) {
    case ::netexec::detail::submit_result::ready:
        return out << "ready";
    case ::netexec::detail::submit_result::submit:
        return out << "submit";
    case ::netexec::detail::submit_result::error:
        return out << "error";
    }
    return out << "<unknown>";
}

// ----------------------------------------------------------------------------
// The struct io_base is used as base class of operation states. Objects of
// this type are also used to kick off the actual work once a readiness
// indication was received.

struct netexec::detail::io_base {
    using extra_t = ::std::unique_ptr<void, auto (*)(void*)->void>;
    using work_t  = auto (*)(::netexec::detail::context_base&, io_base*) -> ::netexec::detail::submit_result;

    io_base*                            next{nullptr}; // used for an intrusive list
    ::netexec::detail::context_base* context{nullptr};
    ::netexec::detail::socket_id     id;    // the entity affected
    ::netexec::event_type            event; // mask for expected events
    work_t                              work;
    extra_t                             extra{nullptr, +[](void*) {}};

    io_base(::netexec::detail::socket_id i, ::netexec::event_type ev) : id(i), event(ev) {}
    virtual ~io_base() = default;

    virtual auto complete() -> void               = 0;
    virtual auto error(::std::error_code) -> void = 0;
    virtual auto cancel() -> void                 = 0;
};

// ----------------------------------------------------------------------------
// The struct io_operation is an io_base storing operation specific data.

template <typename Data>
struct netexec::detail::io_operation : io_base, Data {
    template <typename D = Data>
    io_operation(::netexec::detail::socket_id i, ::netexec::event_type ev, D&& a = Data())
        : io_base(i, ev), Data(::std::forward<D>(a)) {}
};

// ----------------------------------------------------------------------------

#endif
