// include/beman/net/detail/tls/tls_backend.hpp                        -*-C++-*-
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef INCLUDED_INCLUDE_BEMAN_NET_DETAIL_TLS_TLS_BACKEND
#define INCLUDED_INCLUDE_BEMAN_NET_DETAIL_TLS_TLS_BACKEND

// ----------------------------------------------------------------------------
// TLS backend selection macros.
//
// The build system (CMake) may define one of the following to force a backend:
//   - NETEXEC_TLS_USE_SCHANNEL
//   - NETEXEC_TLS_USE_OPENSSL
//   - NETEXEC_TLS_USE_MBEDTLS
//   - NETEXEC_TLS_USE_SECURE_TRANSPORT
//
// If none is defined, we pick a sensible default per platform. Every platform
// gets a well-defined backend macro so that platform-specific code can use
// #if defined(NETEXEC_TLS_USE_*) guards.
// ----------------------------------------------------------------------------

#if defined(NETEXEC_TLS_USE_SCHANNEL)
#  define NETEXEC_TLS_BACKEND_SCHANNEL 1
#elif defined(NETEXEC_TLS_USE_OPENSSL)
#  define NETEXEC_TLS_BACKEND_OPENSSL 1
#elif defined(NETEXEC_TLS_USE_MBEDTLS)
#  define NETEXEC_TLS_BACKEND_MBEDTLS 1
#elif defined(NETEXEC_TLS_USE_SECURE_TRANSPORT)
#  define NETEXEC_TLS_BACKEND_SECURE_TRANSPORT 1
#elif defined(_WIN32)
#  define NETEXEC_TLS_USE_SCHANNEL
#  define NETEXEC_TLS_BACKEND_SCHANNEL 1
#elif defined(__APPLE__)
#  define NETEXEC_TLS_USE_SECURE_TRANSPORT
#  define NETEXEC_TLS_BACKEND_SECURE_TRANSPORT 1
#else
// Default for Linux, Android, and other POSIX systems.
#  define NETEXEC_TLS_USE_OPENSSL
#  define NETEXEC_TLS_BACKEND_OPENSSL 1
#endif

namespace netexec::net::tls::__detail {

// Alias to the concrete backend context type selected above.
// Each backend header defines a class named `<backend>_tls_context`.
#if defined(NETEXEC_TLS_BACKEND_SCHANNEL)
class schannel_tls_context;
using context = schannel_tls_context;
#elif defined(NETEXEC_TLS_BACKEND_OPENSSL)
class openssl_tls_context;
using context = openssl_tls_context;
#elif defined(NETEXEC_TLS_BACKEND_MBEDTLS)
class mbedtls_tls_context;
using context = mbedtls_tls_context;
#elif defined(NETEXEC_TLS_BACKEND_SECURE_TRANSPORT)
class secure_transport_tls_context;
using context = secure_transport_tls_context;
#endif

} // namespace netexec::net::tls::__detail

// ----------------------------------------------------------------------------

#endif
