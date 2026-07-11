// include/beman/net/detail/tls/tls.hpp                               -*-C++-*-
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef INCLUDED_INCLUDE_BEMAN_NET_DETAIL_TLS_TLS
#define INCLUDED_INCLUDE_BEMAN_NET_DETAIL_TLS_TLS

// ----------------------------------------------------------------------------
// Top-level TLS header. Selects the concrete backend based on platform macros
// or CMake-defined NETEXEC_TLS_USE_* macros, and exposes the backend-agnostic
// context/session base classes.
// ----------------------------------------------------------------------------

#include <netexec/net/tls/__detail/tls_backend.hpp>
#include <netexec/net/tls/__detail/tls_context_base.hpp>
#include <netexec/net/tls/__detail/tls_error.hpp>
#include <netexec/net/tls/__detail/tls_session_base.hpp>

#if defined(NETEXEC_TLS_BACKEND_SCHANNEL)
#  include <netexec/net/tls/__detail/schannel_tls.hpp>
#elif defined(NETEXEC_TLS_BACKEND_OPENSSL)
#  include <netexec/net/tls/__detail/openssl_tls.hpp>
#elif defined(NETEXEC_TLS_BACKEND_MBEDTLS)
#  include <netexec/net/tls/__detail/mbedtls_tls.hpp>
#elif defined(NETEXEC_TLS_BACKEND_SECURE_TRANSPORT)
#  include <netexec/net/tls/__detail/secure_transport_tls.hpp>
#else
#  error "No TLS backend selected"
#endif

// ----------------------------------------------------------------------------

#endif
