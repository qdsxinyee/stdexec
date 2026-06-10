// netexec/net.hpp                                                    -*-C++-*-
// Top-level header: async networking built on stdexec.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Usage:
//   #include <netexec/net.hpp>     // everything
// Or pick specific headers:
//   #include <netexec/io_context.hpp>
//   #include <netexec/socket.hpp>
//   etc.
#pragma once

#include <netexec/__detail/basic_socket.hpp>
#include <netexec/__detail/basic_socket_acceptor.hpp>
#include <netexec/__detail/basic_stream_socket.hpp>
#include <netexec/__detail/buffer.hpp>
#include <netexec/__detail/container.hpp>
#include <netexec/__detail/context_base.hpp>
#include <netexec/__detail/endpoint.hpp>
#include <netexec/__detail/internet.hpp>
#include <netexec/__detail/io_base.hpp>
#include <netexec/__detail/io_context.hpp>
#include <netexec/__detail/io_context_scheduler.hpp>
#include <netexec/__detail/netfwd.hpp>
#include <netexec/__detail/operations.hpp>
#include <netexec/__detail/sender.hpp>
#include <netexec/__detail/socket_base.hpp>
#include <netexec/__detail/stop_token.hpp>
#include <netexec/__detail/timer.hpp>
#include <netexec/__detail/scope.hpp>
#include <netexec/task.hpp>
#include <netexec/__detail/initiate.hpp>
#include <netexec/__detail/into_expected.hpp>
#include <netexec/__detail/repeat_effect_until.hpp>
#include <netexec/__detail/listen.hpp>
#include <netexec/__detail/local_endpoint.hpp>
#include <netexec/__detail/preconnection.hpp>
#include <netexec/__detail/remote_endpoint.hpp>
#include <netexec/__detail/rendezvous.hpp>
#include <netexec/__detail/transport_preference.hpp>
