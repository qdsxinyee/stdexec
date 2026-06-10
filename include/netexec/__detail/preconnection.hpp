// include/beman/net/detail/preconnection.hpp                         -*-C++-*-
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef INCLUDED_INCLUDE_BEMAN_NET_DETAIL_PRECONNECTION
#define INCLUDED_INCLUDE_BEMAN_NET_DETAIL_PRECONNECTION

#include <netexec/__detail/local_endpoint.hpp>
#include <netexec/__detail/remote_endpoint.hpp>
#include <netexec/__detail/security_props.hpp>
#include <netexec/__detail/transport_props.hpp>

// ----------------------------------------------------------------------------

namespace netexec::detail {
class preconnection;
}
namespace netexec {
using preconnection = netexec::detail::preconnection;
}

// ----------------------------------------------------------------------------

class netexec::detail::preconnection {
  public:
    preconnection(const remote_endpoint& re, const transport_props& tp = {}, const security_props& sp = {})
        : _remote(re), _local(), _transport_props(tp), _security_props(sp) {
        (void)this->_remote;
        (void)this->_local;
        (void)this->_transport_props;
        (void)this->_security_props;
    }

    auto local() const noexcept -> const local_endpoint& { return this->_local; }
    auto remote() const noexcept -> const remote_endpoint& { return this->_remote; }

  private:
    remote_endpoint _remote;
    local_endpoint  _local;
    transport_props _transport_props;
    security_props  _security_props;
};

// ----------------------------------------------------------------------------

#endif
