// include/beman/net/detail/tls/tls_session_base.hpp                   -*-C++-*-
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef INCLUDED_INCLUDE_BEMAN_NET_DETAIL_TLS_TLS_SESSION_BASE
#define INCLUDED_INCLUDE_BEMAN_NET_DETAIL_TLS_TLS_SESSION_BASE

#include <cstddef>
#include <span>
#include <system_error>

// ----------------------------------------------------------------------------

namespace netexec::net::tls::__detail {

// Backend-agnostic TLS session interface.
// A session represents one TLS connection over an already-established transport
// (e.g. a TCP socket). The higher-level stream drives the handshake by calling
// handshake_step(), sending outgoing_data(), and feeding incoming ciphertext
// with feed_incoming().
class session_base {
  public:
    virtual ~session_base() = default;

    // Perform one step of the TLS handshake.
    // Returns true when the handshake is complete.
    // Returns false when more I/O is needed; in that case the caller must send
    // outgoing_data() to the peer and/or feed more incoming data with feed_incoming().
    virtual auto handshake_step(std::error_code& ec) -> bool = 0;

    // Returns a view of data produced by the handshake that must be sent to the peer.
    // The view remains valid until consume_outgoing() is called or the session is destroyed.
    virtual auto outgoing_data() -> std::span<const std::byte> = 0;

    // Mark `n` bytes of outgoing_data() as consumed (sent).
    virtual auto consume_outgoing(std::size_t n) -> void = 0;

    // Feed ciphertext received from the peer into the handshake or decryption state.
    // `consumed` receives the number of bytes that were consumed from `data`.
    // Any unconsumed bytes belong to the next TLS record and should be fed again later.
    virtual auto feed_incoming(std::span<const std::byte> data, std::size_t& consumed, std::error_code& ec)
        -> void = 0;

    // Gracefully shut down the TLS session.
    virtual auto shutdown(std::error_code& ec) -> void = 0;

    // Largest plaintext chunk that can be passed to encrypt() in one call.
    virtual auto max_message_size() const noexcept -> std::size_t { return 0; }

    // Encrypt `input_size` bytes from `input`. Up to `output_size` encrypted
    // bytes are written to `output`; `output_written` receives the actual count.
    virtual auto encrypt(
        const void* input,
        std::size_t input_size,
        void* output,
        std::size_t output_size,
        std::size_t& output_written,
        std::error_code& ec) -> void = 0;

    // Decrypt `input_size` bytes from `input`. Up to `output_size` plaintext
    // bytes are written to `output`; `output_written` receives the actual count.
    // If a record is incomplete, set ec to EAGAIN-equivalent and output_written to 0.
    virtual auto decrypt(
        const void* input,
        std::size_t input_size,
        void* output,
        std::size_t output_size,
        std::size_t& output_written,
        std::error_code& ec) -> void = 0;
};

} // namespace netexec::net::tls::__detail

// ----------------------------------------------------------------------------

#endif
