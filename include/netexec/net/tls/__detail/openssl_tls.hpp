// include/netexec/net/tls/__detail/openssl_tls.hpp             -*-C++-*-
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef INCLUDED_INCLUDE_NETEXEC_DETAIL_TLS_OPENSSL_TLS
#define INCLUDED_INCLUDE_NETEXEC_DETAIL_TLS_OPENSSL_TLS

#include <netexec/net/tls/__detail/tls_context_base.hpp>
#include <netexec/net/tls/__detail/tls_error.hpp>
#include <netexec/net/tls/__detail/tls_session_base.hpp>

#if !defined(NETEXEC_TLS_BACKEND_OPENSSL)
#  error "openssl_tls.hpp should only be included when NETEXEC_TLS_BACKEND_OPENSSL is defined"
#endif

// Requires OpenSSL 3.0 or newer (EVP_RSA_gen, SSL_set1_host).

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <cstddef>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

// ----------------------------------------------------------------------------

namespace netexec::net::tls::__detail {

namespace openssl {

// Map an OpenSSL error queue entry to the unified tls_errc codes.
inline auto map_openssl_error(unsigned long err) -> std::error_code {
    if (err == 0) {
        return make_error_code(tls_errc::handshake_failed);
    }
    switch (ERR_GET_REASON(err)) {
    case SSL_R_CERTIFICATE_VERIFY_FAILED:
        return make_error_code(tls_errc::certificate_verify_failed);
    default:
        return make_error_code(tls_errc::handshake_failed);
    }
}

// Refine a handshake failure using the certificate chain verification result.
inline auto verify_result_error(SSL* ssl) -> std::error_code {
    switch (SSL_get_verify_result(ssl)) {
    case X509_V_OK:
        return make_error_code(tls_errc::handshake_failed);
    case X509_V_ERR_CERT_HAS_EXPIRED:
    case X509_V_ERR_CERT_NOT_YET_VALID:
        return make_error_code(tls_errc::certificate_expired);
    case X509_V_ERR_HOSTNAME_MISMATCH:
        return make_error_code(tls_errc::certificate_name_mismatch);
    case X509_V_ERR_CERT_REVOKED:
        return make_error_code(tls_errc::certificate_revoked);
    case X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT:
    case X509_V_ERR_SELF_SIGNED_CERT_IN_CHAIN:
    case X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT_LOCALLY:
    case X509_V_ERR_UNABLE_TO_VERIFY_LEAF_SIGNATURE:
    case X509_V_ERR_CERT_UNTRUSTED:
        return make_error_code(tls_errc::certificate_untrusted_root);
    default:
        return make_error_code(tls_errc::certificate_verify_failed);
    }
}

} // namespace openssl

// ----------------------------------------------------------------------------
// OpenSSL TLS session.
//
// The session never touches a socket.  It is driven by the higher-level
// stream exactly like the Schannel backend:
//   1. Call handshake_step() until it returns true.
//   2. Between calls, send outgoing_data() to the peer and feed incoming
//      ciphertext with feed_incoming().
//   3. After the handshake, use encrypt() / decrypt() for application data.
//
// Internally a pair of memory BIOs decouples the TLS state machine from the
// asynchronous I/O: ciphertext received from the socket is written into
// rbio_, and ciphertext OpenSSL produces (handshake messages, encrypted
// records, close_notify) is drained from wbio_.
// ----------------------------------------------------------------------------
class openssl_tls_session : public session_base {
  public:
    // Client-side constructor.  `hostname` is used for SNI and certificate
    // name verification; it may be empty (no SNI, no name check).
    openssl_tls_session(SSL_CTX* ctx, std::string hostname)
        : ssl_(SSL_new(ctx))
        , is_client_(true) {
        BIO* rbio = BIO_new(BIO_s_mem());
        BIO* wbio = BIO_new(BIO_s_mem());
        SSL_set_bio(this->ssl_, rbio, wbio); // ssl_ takes ownership of both
        SSL_set_connect_state(this->ssl_);
        SSL_set_verify(this->ssl_, SSL_VERIFY_PEER, nullptr);
        if (!hostname.empty()) {
            SSL_set_tlsext_host_name(this->ssl_, hostname.c_str());
            SSL_set1_host(this->ssl_, hostname.c_str());
        }
    }

    // Server-side constructor.
    explicit openssl_tls_session(SSL_CTX* ctx)
        : ssl_(SSL_new(ctx))
        , is_client_(false) {
        BIO* rbio = BIO_new(BIO_s_mem());
        BIO* wbio = BIO_new(BIO_s_mem());
        SSL_set_bio(this->ssl_, rbio, wbio);
        SSL_set_accept_state(this->ssl_);
    }

    ~openssl_tls_session() {
        if (this->ssl_ != nullptr) {
            SSL_free(this->ssl_); // also frees the memory BIOs
        }
    }

    openssl_tls_session(const openssl_tls_session&)            = delete;
    openssl_tls_session& operator=(const openssl_tls_session&) = delete;

    auto handshake_step(std::error_code& ec) -> bool override {
        if (this->handshake_complete_) {
            ec.clear();
            return true;
        }

        ERR_clear_error();
        const int ret = SSL_do_handshake(this->ssl_);
        this->drain_outgoing();

        if (ret == 1) {
            this->handshake_complete_ = true;
            ec.clear();
            return true;
        }

        const int ssl_err = SSL_get_error(this->ssl_, ret);
        if (ssl_err == SSL_ERROR_WANT_READ || ssl_err == SSL_ERROR_WANT_WRITE) {
            ec.clear();
            return false;
        }
        if (ssl_err == SSL_ERROR_ZERO_RETURN) {
            ec = make_error_code(tls_errc::unexpected_eof);
            return false;
        }
        if (ssl_err == SSL_ERROR_SSL) {
            ec = openssl::verify_result_error(this->ssl_);
            return false;
        }
        ec = openssl::map_openssl_error(ERR_get_error());
        return false;
    }

    auto outgoing_data() -> std::span<const std::byte> override {
        if (this->output_buffer_.empty()) {
            return {};
        }
        return std::span{reinterpret_cast<const std::byte*>(this->output_buffer_.data()),
                         this->output_buffer_.size()};
    }

    auto consume_outgoing(std::size_t n) -> void override {
        if (n >= this->output_buffer_.size()) {
            this->output_buffer_.clear();
        } else {
            this->output_buffer_.erase(this->output_buffer_.begin(),
                                       this->output_buffer_.begin() + static_cast<std::ptrdiff_t>(n));
        }
    }

    auto feed_incoming(std::span<const std::byte> data, std::size_t& consumed, std::error_code& ec)
        -> void override {
        consumed = 0;
        if (!data.empty()) {
            const int n = BIO_write(this->rbio(), data.data(), static_cast<int>(data.size()));
            consumed    = n > 0 ? static_cast<std::size_t>(n) : 0;
        }
        ec.clear();
    }

    auto shutdown(std::error_code& ec) -> void override {
        if (!this->handshake_complete_ || this->shutdown_sent_) {
            ec.clear();
            return;
        }
        ERR_clear_error();
        // Queue close_notify into wbio_.  We do not wait for the peer's
        // close_notify; the caller just sends whatever we produce.
        SSL_shutdown(this->ssl_);
        this->drain_outgoing();
        this->shutdown_sent_ = true;
        ec.clear();
    }

    auto max_message_size() const noexcept -> std::size_t override { return 16384; }

    auto encrypt(
        const void* input,
        std::size_t input_size,
        void* output,
        std::size_t output_size,
        std::size_t& output_written,
        std::error_code& ec) -> void override {
        output_written = 0;

        if (!this->handshake_complete_) {
            ec = make_error_code(tls_errc::handshake_failed);
            return;
        }
        if (input_size > this->max_message_size()) {
            ec = make_error_code(tls_errc::message_too_large);
            return;
        }

        ERR_clear_error();
        const int ret = SSL_write(this->ssl_, input, static_cast<int>(input_size));
        if (ret <= 0) {
            ec = make_error_code(tls_errc::encryption_failed);
            return;
        }

        // One TLS record is at most ~16 KiB plus a small overhead, well below
        // the 64 KiB buffer used by the caller; check anyway.
        const std::size_t pending = BIO_ctrl_pending(this->wbio());
        if (pending > output_size) {
            ec = make_error_code(tls_errc::no_buffer_space);
            return;
        }
        if (pending != 0) {
            const int n = BIO_read(this->wbio(), output, static_cast<int>(pending));
            output_written = n > 0 ? static_cast<std::size_t>(n) : 0;
        }
        ec.clear();
    }

    auto decrypt(
        const void* input,
        std::size_t input_size,
        void* output,
        std::size_t output_size,
        std::size_t& output_written,
        std::error_code& ec) -> void override {
        output_written = 0;

        if (!this->handshake_complete_) {
            ec = make_error_code(tls_errc::handshake_failed);
            return;
        }

        if (input != nullptr && input_size != 0) {
            BIO_write(this->rbio(), input, static_cast<int>(input_size));
        }

        ERR_clear_error();
        const int ret = SSL_read(this->ssl_, output, static_cast<int>(output_size));
        if (ret > 0) {
            output_written = static_cast<std::size_t>(ret);
            ec.clear();
            return;
        }

        const int ssl_err = SSL_get_error(this->ssl_, ret);
        switch (ssl_err) {
        case SSL_ERROR_WANT_READ:
        case SSL_ERROR_WANT_WRITE:
            // Not enough ciphertext for a complete record; the caller must
            // read more from the socket.
            ec = std::make_error_code(std::errc::resource_unavailable_try_again);
            return;
        case SSL_ERROR_ZERO_RETURN:
            // Peer sent close_notify: graceful EOF.
            ec.clear();
            return;
        case SSL_ERROR_SYSCALL:
            if (ret == 0 || ERR_peek_error() == 0) {
                // Abrupt transport close without close_notify; treat as EOF.
                ec.clear();
                return;
            }
            ec = make_error_code(tls_errc::decryption_failed);
            return;
        case SSL_ERROR_SSL:
            if (ERR_GET_REASON(ERR_peek_error()) == SSL_R_UNEXPECTED_EOF_WHILE_READING) {
                // OpenSSL 3.x reports a missing close_notify this way.
                ec.clear();
                return;
            }
            ec = make_error_code(tls_errc::decryption_failed);
            return;
        default:
            ec = make_error_code(tls_errc::decryption_failed);
            return;
        }
    }

  private:
    auto rbio() -> BIO* { return SSL_get_rbio(this->ssl_); }
    auto wbio() -> BIO* { return SSL_get_wbio(this->ssl_); }

    // Move all ciphertext OpenSSL has produced into output_buffer_.
    auto drain_outgoing() -> void {
        unsigned char buf[16384];
        int           n;
        while ((n = BIO_read(this->wbio(), buf, static_cast<int>(sizeof(buf)))) > 0) {
            this->output_buffer_.insert(this->output_buffer_.end(), buf, buf + n);
        }
    }

    SSL*                        ssl_;
    bool                        is_client_;
    bool                        handshake_complete_ = false;
    bool                        shutdown_sent_      = false;
    std::vector<unsigned char>  output_buffer_;
};

// ----------------------------------------------------------------------------
// OpenSSL TLS context: owns the SSL_CTX, loads certificates/keys/CA bundles,
// and creates sessions.  When no certificate was provided, the server side
// generates an in-memory self-signed certificate (mirroring the Schannel
// backend) so local HTTPS works out of the box.
// ----------------------------------------------------------------------------
class openssl_tls_context : public context_base {
  public:
    openssl_tls_context()
        : ctx_(SSL_CTX_new(TLS_method())) {
        if (this->ctx_ != nullptr) {
            // TLS 1.0/1.1 are disabled by all major browsers; require >= 1.2.
            SSL_CTX_set_min_proto_version(this->ctx_, TLS1_2_VERSION);
        }
    }

    ~openssl_tls_context() override {
        if (this->ctx_ != nullptr) {
            SSL_CTX_free(this->ctx_);
        }
    }

    openssl_tls_context(const openssl_tls_context&)            = delete;
    openssl_tls_context& operator=(const openssl_tls_context&) = delete;

    auto use_certificate_file(std::string_view path) -> std::error_code override {
        if (this->ctx_ == nullptr) {
            return make_error_code(tls_errc::unsupported_operation);
        }
        const std::string p{path};
        if (SSL_CTX_use_certificate_chain_file(this->ctx_, p.c_str()) != 1) {
            return openssl::map_openssl_error(ERR_get_error());
        }
        this->cert_loaded_ = true;
        return {};
    }

    auto use_private_key_file(std::string_view path) -> std::error_code override {
        if (this->ctx_ == nullptr) {
            return make_error_code(tls_errc::unsupported_operation);
        }
        const std::string p{path};
        if (SSL_CTX_use_PrivateKey_file(this->ctx_, p.c_str(), SSL_FILETYPE_PEM) != 1) {
            return openssl::map_openssl_error(ERR_get_error());
        }
        if (SSL_CTX_check_private_key(this->ctx_) != 1) {
            return openssl::map_openssl_error(ERR_get_error());
        }
        return {};
    }

    auto use_ca_bundle(std::string_view path) -> std::error_code override {
        if (this->ctx_ == nullptr) {
            return make_error_code(tls_errc::unsupported_operation);
        }
        const std::string p{path};
        if (SSL_CTX_load_verify_locations(this->ctx_, p.c_str(), nullptr) != 1) {
            return openssl::map_openssl_error(ERR_get_error());
        }
        return {};
    }

    auto use_default_trust_store() -> std::error_code override {
        if (this->ctx_ == nullptr) {
            return make_error_code(tls_errc::unsupported_operation);
        }
        if (SSL_CTX_set_default_verify_paths(this->ctx_) != 1) {
            return openssl::map_openssl_error(ERR_get_error());
        }
        return {};
    }

    auto set_hostname(std::string_view name) -> void override { this->hostname_ = std::string{name}; }

    auto create_client_session() -> std::unique_ptr<session_base> override {
        if (this->ctx_ == nullptr) {
            return nullptr;
        }
        return std::make_unique<openssl_tls_session>(this->ctx_, this->hostname_);
    }

    auto create_server_session() -> std::unique_ptr<session_base> override {
        if (this->ctx_ == nullptr) {
            return nullptr;
        }
        if (!this->cert_loaded_) {
            if (this->generate_self_signed_certificate()) {
                return nullptr;
            }
        }
        return std::make_unique<openssl_tls_session>(this->ctx_);
    }

  private:
    // Generate an in-memory self-signed certificate (RSA 2048, SHA-256, SAN
    // covering the configured hostname plus localhost/127.0.0.1/::1) and
    // install it into the SSL_CTX.  Mirrors the Schannel backend behavior.
    auto generate_self_signed_certificate() -> std::error_code {
        const std::string cn = this->hostname_.empty() ? "localhost" : this->hostname_;

        std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> pkey(EVP_RSA_gen(2048), &EVP_PKEY_free);
        if (!pkey) {
            return make_error_code(tls_errc::invalid_argument);
        }

        std::unique_ptr<X509, decltype(&X509_free)> cert(X509_new(), &X509_free);
        if (!cert) {
            return make_error_code(tls_errc::invalid_argument);
        }

        X509_set_version(cert.get(), X509_VERSION_3);

        // Random positive 63-bit serial number.
        std::int64_t serial = 0;
        if (RAND_bytes(reinterpret_cast<unsigned char*>(&serial), sizeof(serial)) != 1) {
            return make_error_code(tls_errc::invalid_argument);
        }
        serial &= 0x7FFFFFFFFFFFFFFFLL;
        ASN1_INTEGER_set(X509_get_serialNumber(cert.get()), static_cast<long>(serial));

        // Valid from one hour ago (clock-skew tolerance) for one year.
        X509_gmtime_adj(X509_getm_notBefore(cert.get()), -3600);
        X509_gmtime_adj(X509_getm_notAfter(cert.get()), 365L * 24 * 3600);

        X509_NAME* name = X509_get_subject_name(cert.get());
        if (X509_NAME_add_entry_by_txt(
                name,
                "CN",
                MBSTRING_ASC,
                reinterpret_cast<const unsigned char*>(cn.c_str()),
                -1,
                -1,
                0)
            != 1) {
            return make_error_code(tls_errc::invalid_argument);
        }
        // Self-signed: issuer == subject.
        if (X509_set_issuer_name(cert.get(), name) != 1) {
            return make_error_code(tls_errc::invalid_argument);
        }
        if (X509_set_pubkey(cert.get(), pkey.get()) != 1) {
            return make_error_code(tls_errc::invalid_argument);
        }

        X509V3_CTX ext_ctx;
        X509V3_set_ctx_nodb(&ext_ctx);
        X509V3_set_ctx(&ext_ctx, cert.get(), cert.get(), nullptr, nullptr, 0);

        const auto add_ext = [&](int nid, const char* value) -> bool {
            X509_EXTENSION* ext = X509V3_EXT_conf_nid(nullptr, &ext_ctx, nid, value);
            if (ext == nullptr) {
                return false;
            }
            const bool ok = X509_add_ext(cert.get(), ext, -1) == 1;
            X509_EXTENSION_free(ext);
            return ok;
        };

        if (!add_ext(NID_basic_constraints, "critical,CA:FALSE")) {
            return make_error_code(tls_errc::invalid_argument);
        }

        // Browsers only honor the SAN, not the CN.  Cover the configured
        // hostname plus the loopback addresses, like the Schannel backend.
        std::string san = "DNS:" + cn;
        if (cn != "localhost") {
            san += ",DNS:localhost";
        }
        san += ",IP:127.0.0.1,IP:::1";
        if (!add_ext(NID_subject_alt_name, san.c_str())) {
            return make_error_code(tls_errc::invalid_argument);
        }

        if (!add_ext(NID_key_usage, "critical,digitalSignature,keyEncipherment")) {
            return make_error_code(tls_errc::invalid_argument);
        }
        if (!add_ext(NID_ext_key_usage, "serverAuth")) {
            return make_error_code(tls_errc::invalid_argument);
        }

        // Self-sign with the freshly generated key.
        if (X509_sign(cert.get(), pkey.get(), EVP_sha256()) == 0) {
            return make_error_code(tls_errc::invalid_argument);
        }

        if (SSL_CTX_use_certificate(this->ctx_, cert.get()) != 1) {
            return openssl::map_openssl_error(ERR_get_error());
        }
        if (SSL_CTX_use_PrivateKey(this->ctx_, pkey.get()) != 1) {
            return openssl::map_openssl_error(ERR_get_error());
        }
        if (SSL_CTX_check_private_key(this->ctx_) != 1) {
            return openssl::map_openssl_error(ERR_get_error());
        }

        this->cert_loaded_ = true;
        return {};
    }

    SSL_CTX*    ctx_;
    std::string hostname_;
    bool        cert_loaded_ = false;
};

} // namespace netexec::net::tls::__detail

// ----------------------------------------------------------------------------

#endif
