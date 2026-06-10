// include/beman/net/detail/initiate.hpp                              -*-C++-*-
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef INCLUDED_INCLUDE_BEMAN_NET_DETAIL_INITIATE
#define INCLUDED_INCLUDE_BEMAN_NET_DETAIL_INITIATE
#include <netexec/__detail/netexec_detail.hpp>

#include <netexec/__detail/operations.hpp>
#include <netexec/__detail/preconnection.hpp>
#include <netexec/__detail/internet.hpp>
#include <netexec/__detail/into_expected.hpp>
#include <exec/task.hpp>

#include <system_error>
#include <utility>

// ----------------------------------------------------------------------------

namespace netexec::detail {
class initiate_t {
  public:
    // Connect to the endpoint described by `pre`, using `ctx` as the I/O context.
    // The io_context must be passed explicitly because exec::basic_task's default
    // context does not propagate custom env queries such as get_io_handle.
    auto operator()(const preconnection& pre, netexec::io_context& ctx) const
        -> netexec::task<netexec::ip::tcp::socket> {
        netexec::ip::tcp::endpoint ep(netexec::ip::address_v4::loopback(), pre.remote().port());
        netexec::ip::tcp::socket client(ctx, ep);

        auto exp{co_await (netexec::async_connect(client) | netexec::detail::into_expected |
                           stdexec::then([](auto&& e) { return std::move(e); }))};
        if (!exp) {
            // Propagate the error as a system_error exception.
            // exec::basic_task catches it and signals set_error(exception_ptr).
            throw std::system_error(exp.error());
        }
        co_return std::move(client);
    }
};
} // namespace netexec::detail

namespace netexec {
using initiate_t = netexec::detail::initiate_t;
inline constexpr initiate_t initiate{};
} // namespace netexec

// ----------------------------------------------------------------------------

#endif
