// netexec/net/tls/operations.hpp                                        -*-C++-*-
// High-level TLS CPOs built on top of the low-level socket API.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
#pragma once

#include <netexec/net/buffer.hpp>
#include <netexec/net/ip/address.hpp>
#include <netexec/net/ip/tcp/operations.hpp>
#include <netexec/net/ip/tcp/socket.hpp>
#include <netexec/net/message.hpp>
#include <netexec/net/tls/acceptor.hpp>
#include <netexec/net/tls/preconnection.hpp>
#include <netexec/net/tls/stream.hpp>

#include <exec/task.hpp>
#ifndef _MSC_VER
#include <netdb.h>
#endif
#include <exception>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>

namespace netexec::net::tls {

namespace __detail {

inline auto run_handshake(ip::tcp::socket& sock, __detail::session_base& session)
    -> exec::task<void> {
    char readbuf[8192];
    while (true) {
        std::error_code ec;
        bool            complete = session.handshake_step(ec);

        // Send any handshake data produced by this step.
        auto outgoing = session.outgoing_data();
        while (!outgoing.empty()) {
            const auto* out_bytes = reinterpret_cast<const char*>(outgoing.data());
            std::size_t out_sent  = 0;
            while (out_sent < outgoing.size()) {
                auto n = co_await ip::tcp::async_send(
                    sock, net::buffer(out_bytes + out_sent, outgoing.size() - out_sent));
                out_sent += n;
            }
            session.consume_outgoing(outgoing.size());
            outgoing = session.outgoing_data();
        }

        if (complete) {
            co_return;
        }

        if (ec) {
            throw std::system_error(ec);
        }

        // Need more incoming data.
        auto received = co_await ip::tcp::async_receive(sock, net::buffer(readbuf));
        if (received == 0) {
            throw std::runtime_error("TLS handshake failed: connection closed prematurely");
        }

        std::size_t consumed = 0;
        session.feed_incoming(
            std::span{reinterpret_cast<const std::byte*>(readbuf), received},
            consumed,
            ec);
        if (ec) {
            throw std::system_error(ec);
        }
        (void)consumed;
    }
}

} // namespace __detail

inline constexpr struct async_initiate_t {
    auto operator()(const preconnection& pre, io_context& ctx) const
        -> exec::task<stream> {
        std::optional<ip::tcp::socket> sock;
        if (!pre.ip_address().empty()) {
            sock.emplace(ctx, ip::tcp::endpoint(ip::make_address(pre.ip_address()), pre.port()));
            co_await ip::tcp::async_connect(*sock);
        } else if (!pre.hostname().empty()) {
            auto endpoints = co_await net::ip::tcp::async_resolve(ctx, pre.hostname(), pre.port());
            if (endpoints.empty()) {
                throw std::runtime_error("failed to resolve " + pre.hostname());
            }
            std::exception_ptr last_error;
            for (const auto& candidate : endpoints) {
                try {
                    sock.emplace(ctx, candidate);
                    co_await ip::tcp::async_connect(*sock);
                    break;
                } catch (...) {
                    last_error = std::current_exception();
                    sock.reset();
                }
            }
            if (!sock) {
                if (last_error) {
                    std::rethrow_exception(last_error);
                }
                throw std::runtime_error("failed to connect to " + pre.hostname());
            }
        } else {
            sock.emplace(ctx, ip::tcp::endpoint(ip::address_v4::loopback(), pre.port()));
            co_await ip::tcp::async_connect(*sock);
        }

        std::unique_ptr<__detail::session_base> session;
        if (pre.secure()) {
            auto context = pre.make_context();
            if (!context) {
                throw std::runtime_error("failed to create TLS context");
            }
            session = context->create_client_session();
            if (!session) {
                throw std::runtime_error("failed to create TLS client session");
            }
            co_await __detail::run_handshake(*sock, *session);
        }

        co_return stream(std::move(*sock), std::move(session));
    }
} async_initiate{};

inline constexpr struct async_listen_t {
    auto operator()(const preconnection& pre, io_context& ctx) const
        -> exec::task<acceptor> {
        ip::tcp::endpoint ep = pre.ip_address().empty()
            ? ip::tcp::endpoint(ip::address_v4::any(), pre.port())
            : ip::tcp::endpoint(ip::make_address(pre.ip_address()), pre.port());
        ip::tcp::acceptor acc(ctx, ep.protocol());
        co_await ip::tcp::async_listen(acc, ep);

        std::unique_ptr<__detail::context_base> context;
        if (pre.secure()) {
            context = pre.make_context();
            if (!context) {
                throw std::runtime_error("failed to create TLS context");
            }
            // Warm up the context now so certificate generation / credential
            // acquisition happens at listen time rather than on the first client.
            if (!context->create_server_session()) {
                throw std::runtime_error("failed to warm up TLS context");
            }
        }

        co_return acceptor(std::move(acc), std::move(context));
    }
} async_listen{};

inline constexpr struct async_rendezvous_t {
    auto operator()(const preconnection&, io_context&) const -> exec::task<void> {
        throw std::runtime_error("async_rendezvous not implemented");
    }
} async_rendezvous{};

inline constexpr struct async_accept_t {
    auto operator()(acceptor& acc) const -> exec::task<stream> {
        auto [sock, endpoint] = co_await ip::tcp::async_accept(acc.base());

        std::unique_ptr<__detail::session_base> session;
        if (acc.secure()) {
            auto* context = acc.context();
            if (!context) {
                throw std::runtime_error("TLS acceptor has no context");
            }
            session = context->create_server_session();
            if (!session) {
                throw std::runtime_error("failed to create TLS server session");
            }
            try {
                co_await __detail::run_handshake(sock, *session);
            } catch (const std::exception& e) {
                std::ostringstream msg;
                msg << "TLS handshake failed for " << endpoint << ": " << e.what();
                throw std::runtime_error(msg.str());
            }
        }

        co_return stream(std::move(sock), std::move(session));
    }
} async_accept{};

inline constexpr struct async_send_t {
    auto operator()(stream& strm, const message& msg) const -> exec::task<void> {
        if (!strm.secure()) {
            // Plain socket path.
            std::size_t sent = 0;
            while (sent < msg.size()) {
                auto n = co_await ip::tcp::async_send(
                    strm.socket(),
                    net::buffer(reinterpret_cast<const char*>(msg.data()) + sent,
                                msg.size() - sent));
                sent += n;
            }
            co_return;
        }

        // TLS path: encrypt in chunks and send the ciphertext.
        auto*       session = strm.session();
        std::size_t sent    = 0;
        while (sent < msg.size()) {
            const std::size_t chunk_size = std::min(
                msg.size() - sent,
                session->max_message_size() == 0 ? static_cast<std::size_t>(16384)
                                                 : session->max_message_size());

            alignas(16) char ciphertext[65536];
            std::size_t      written = 0;
            std::error_code  ec;
            session->encrypt(
                msg.data() + sent,
                chunk_size,
                ciphertext,
                sizeof(ciphertext),
                written,
                ec);
            if (ec) {
                throw std::system_error(ec);
            }

            const auto* ct_bytes = static_cast<const char*>(ciphertext);
            std::size_t ct_sent    = 0;
            while (ct_sent < written) {
                auto n = co_await ip::tcp::async_send(
                    strm.socket(),
                    net::buffer(ct_bytes + ct_sent, written - ct_sent));
                ct_sent += n;
            }
            sent += chunk_size;
        }
    }
} async_send{};

inline constexpr struct async_shutdown_t {
    // Gracefully shut down a TLS session.  For a secure stream this sends a
    // TLS close_notify to the peer; for a plaintext stream it is a no-op.
    // The caller is still responsible for closing the underlying socket
    // afterwards (usually by destroying the stream).
    auto operator()(stream& strm) const -> exec::task<void> {
        if (!strm.secure()) {
            co_return;
        }

        auto* session = strm.session();
        std::error_code ec;
        session->shutdown(ec);
        if (ec) {
            throw std::system_error(ec);
        }

        while (true) {
            auto outgoing = session->outgoing_data();
            if (outgoing.empty()) {
                break;
            }
            const auto* out_bytes = reinterpret_cast<const char*>(outgoing.data());
            std::size_t out_sent    = 0;
            while (out_sent < outgoing.size()) {
                auto n = co_await ip::tcp::async_send(
                    strm.socket(),
                    net::buffer(out_bytes + out_sent, outgoing.size() - out_sent));
                out_sent += n;
            }
            session->consume_outgoing(outgoing.size());
        }
    }
} async_shutdown{};

inline constexpr struct async_receive_some_t {
    // Read and decrypt up to `buffer` bytes of plaintext.  Returns the number
    // of plaintext bytes written (zero means the peer closed the connection).
    auto operator()(stream& strm, net::mutable_buffer buffer) const -> exec::task<std::size_t> {
        if (!strm.secure()) {
            co_return co_await ip::tcp::async_receive(strm.socket(), buffer);
        }

        auto*       session    = strm.session();
        char        ciphertext[8192];
        const auto  try_again  = std::make_error_code(std::errc::resource_unavailable_try_again);

        while (true) {
            auto*       iov     = buffer.data();
            std::size_t written = 0;
            std::error_code ec;
            session->decrypt(nullptr, 0, iov->iov_base, iov->iov_len, written, ec);
            if (!ec) {
                if (written != 0) {
                    co_return written;
                }
                // No buffered plaintext; need more ciphertext.
            } else if (ec != try_again) {
                throw std::system_error(ec);
            }

            auto received = co_await ip::tcp::async_receive(strm.socket(), net::buffer(ciphertext));
            if (received == 0) {
                co_return 0;
            }

            session->decrypt(ciphertext, received, iov->iov_base, iov->iov_len, written, ec);
            if (!ec) {
                if (written != 0) {
                    co_return written;
                }
                // Decrypt succeeded but produced no data; there may be more
                // records buffered, so loop and try again.
            } else if (ec == try_again) {
                continue;
            } else {
                throw std::system_error(ec);
            }
        }
    }
} async_receive_some{};

inline constexpr struct async_receive_t {
    // Phase 3 simplified implementation: read until EOF or connection close.
    // Future versions will use framers/TLS to determine message boundaries.
    auto operator()(stream& strm) const -> exec::task<message> {
        if (!strm.secure()) {
            // Plain socket path.
            std::vector<std::byte> data;
            char buf[1024];
            while (true) {
                auto n = co_await ip::tcp::async_receive(strm.socket(), net::buffer(buf));
                if (n == 0) {
                    break;
                }
                data.insert(data.end(),
                            reinterpret_cast<std::byte*>(buf),
                            reinterpret_cast<std::byte*>(buf) + n);
            }
            co_return message(std::move(data));
        }

        // TLS path: receive ciphertext and decrypt.
        auto*                  session   = strm.session();
        std::vector<std::byte> plaintext;
        char                   ciphertext[8192];
        bool                   eof = false;

        while (true) {
            std::size_t received = 0;
            if (!eof) {
                received = co_await ip::tcp::async_receive(strm.socket(), net::buffer(ciphertext));
                if (received == 0) {
                    eof = true;
                }
            }

            // Attempt to decrypt one or more TLS records.  The first call may
            // feed new ciphertext; subsequent calls drain complete records
            // already buffered inside the session.
            bool fed_data = (received != 0);
            while (true) {
                char        decrypted[8192];
                std::size_t written = 0;
                std::error_code ec;
                session->decrypt(
                    fed_data ? ciphertext : nullptr,
                    fed_data ? received : 0,
                    decrypted,
                    sizeof(decrypted),
                    written,
                    ec);
                fed_data = false;

                if (ec) {
                    if (ec == std::make_error_code(std::errc::resource_unavailable_try_again)) {
                        // Need more ciphertext; keep reading unless the socket
                        // has already reported EOF.
                        if (eof) {
                            throw std::system_error(ec);
                        }
                        break;
                    }
                    throw std::system_error(ec);
                }

                if (written == 0) {
                    break;
                }

                plaintext.insert(plaintext.end(),
                                 reinterpret_cast<std::byte*>(decrypted),
                                 reinterpret_cast<std::byte*>(decrypted) + written);
            }

            if (eof) {
                break;
            }
        }

        co_return message(std::move(plaintext));
    }
} async_receive{};

} // namespace netexec::net::tls
